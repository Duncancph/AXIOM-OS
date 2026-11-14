# Makes the key pair for signing AXIOM updates (ECDSA P-256).
# The private key stays with whoever publishes updates; the public key is built into the kernel (src/update_key.h).
import sys
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
priv_path = sys.argv[1]
k = ec.generate_private_key(ec.SECP256R1())
open(priv_path, "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
pub = k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
lines = ["/* Public key that AXIOM updates must be signed with (ECDSA P-256, uncompressed point). Made by tools/update-keygen.py. */",
         "#pragma once", "static const unsigned char update_pubkey[65] = {"]
for i in range(0, 65, 13): lines.append("    " + ", ".join("0x%02x" % b for b in pub[i:i + 13]) + ",")
lines.append("};")
open("src/update_key.h", "w").write("\n".join(lines) + "\n")
print("private key:", priv_path, "\npublic key: src/update_key.h")
