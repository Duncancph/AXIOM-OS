#!/bin/bash
# TLS 1.3 client test matrix: runs tests/test_tls.c against "openssl s_server" with good and bad certificates.
# Usage: matrix.sh <test_tls binary> <work dir>
set -u
T=$(realpath "$1"); W=$2; HERE=$(dirname "$(realpath "$0")")
mkdir -p "$W" && cd "$W"
[ -f ca.pem ] || python3 "$HERE/mkpki.py"
[ -f big.bin ] || head -c 3000000 /dev/urandom > big.bin
BIG=$(sha256sum big.bin | cut -c1-16)
# name | expect | server args | client host | path
run() {
  local name="$1" expect="$2" args="$3" host="${4:-duncos.test}" path="${5:-/}"
  local port=$((44300 + RANDOM % 1000))
  openssl s_server -quiet -naccept 1 -accept $port $args >/dev/null 2>&1 &
  local pid=$!; sleep 0.4
  local out; out=$(timeout 30 $T ca.pem "$host" $port "$path" 2>&1 | grep RESULT)
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  if [[ "$out" == *"$expect"* ]]; then echo "PASS  $name"; else echo "FAIL  $name -> $out"; FAILS=$((FAILS+1)); fi
  [ -n "${VERBOSE:-}" ] && echo "      $out"
}
FAILS=0
RSA="-cert rsa_leaf.pem -key rsa_leaf.key -cert_chain rsa_int.pem"
EC="-cert ec_leaf.pem -key ec_leaf.key -cert_chain ec_int.pem"
EC384="-cert ec384_leaf.pem -key ec384_leaf.key -cert_chain ec_int.pem"
run "RSA chain, defaults (www)"             "RESULT ok" "$RSA -www -tls1_3"
run "RSA, AES-128-GCM only"                 "AES-128-GCM" "$RSA -www -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256"
run "RSA, ChaCha20-Poly1305 only"           "ChaCha20" "$RSA -www -tls1_3 -ciphersuites TLS_CHACHA20_POLY1305_SHA256"
run "RSA, P-256 only (HelloRetryRequest)"   "P-256" "$RSA -www -tls1_3 -groups P-256"
run "RSA, X25519"                           "X25519" "$RSA -www -tls1_3 -groups X25519"
run "ECDSA P-256 leaf, P-384 chain"         "RESULT ok" "$EC -www -tls1_3"
run "ECDSA P-384 leaf"                      "RESULT ok" "$EC384 -www -tls1_3"
run "ECDSA, P-256 key exchange + AES"       "AES-128-GCM, P-256" "$EC -www -tls1_3 -groups P-256 -ciphersuites TLS_AES_128_GCM_SHA256"
run "wildcard name www.duncos.test"         "RESULT ok" "$RSA -www -tls1_3" "www.duncos.test"
run "IP address 127.0.0.1 (SAN IP)"         "RESULT ok" "$RSA -www -tls1_3" "127.0.0.1"
run "3 MB download, integrity (RSA)"        "$BIG" "$RSA -WWW -tls1_3" "duncos.test" "/big.bin"
run "3 MB download, integrity (ChaCha/EC)"  "$BIG" "$EC -WWW -tls1_3 -ciphersuites TLS_CHACHA20_POLY1305_SHA256" "duncos.test" "/big.bin"
run "client certificate requested"         "RESULT ok" "$RSA -www -tls1_3 -verify 1"
run "REJECT expired certificate"           "expired" "-cert expired_leaf.pem -key expired_leaf.key -cert_chain rsa_int.pem -www -tls1_3"
run "REJECT wrong host name"                "for 'other.test'" "-cert other_leaf.pem -key other_leaf.key -cert_chain rsa_int.pem -www -tls1_3" "duncos.test"
run "REJECT name not in certificate"        "not this site" "$RSA -www -tls1_3" "evil.test"
run "REJECT wildcard two levels deep"       "not this site" "$RSA -www -tls1_3" "a.b.duncos.test"
run "REJECT self-signed certificate"        "doesn't trust" "-cert self_leaf.pem -key self_leaf.key -www -tls1_3"
run "REJECT missing intermediate"           "doesn't trust" "-cert rsa_leaf.pem -key rsa_leaf.key -www -tls1_3"
run "REJECT client-only certificate"        "isn't meant for web servers" "-cert client_leaf.pem -key client_leaf.key -cert_chain rsa_int.pem -www -tls1_3"
run "REJECT TLS 1.2-only server"            "doesn't support TLS 1.3" "$RSA -www -tls1_2"
echo "TLS matrix failures: $FAILS"
[ $FAILS -eq 0 ]
