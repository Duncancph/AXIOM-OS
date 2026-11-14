# Checks common/inet.c against Python's ipaddress module.
import ipaddress, random, subprocess, sys
R = random.Random(7)
cases = ["::", "::1", "1::", "2001:db8::1", "2001:DB8:0:0:1:0:0:1", "2001:db8:0:1:1:1:1:1", "fe80::1%eth0",
         "::ffff:192.0.2.1", "64:ff9b::192.0.2.33", "1:2:3:4:5:6:7:8", "1:0:0:2:0:0:0:3", "0:0:1:0:0:0:0:0",
         "192.0.2.1", "255.255.255.255", "0.0.0.0", "1.2.3", "256.1.1.1", "01.2.3.4", "1.2.3.4.5", ":", ":::", "1::2::3",
         "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7::8", "12345::", "g::", "::1.2.3.4.5", "1:2:3:4:5:6:1.2.3.4", "1:2:3:4:5:6:7:1.2.3.4",
         "::1:", ":1::", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "", "fe80::"]
for _ in range(3000):
    g = [R.choice([0, 0, 0, R.randrange(65536), R.randrange(16)]) for _ in range(8)]
    cases.append(str(ipaddress.IPv6Address(bytes(b for x in g for b in x.to_bytes(2, "big")))))
    cases.append(":".join("%x" % x for x in g))
p = subprocess.run([sys.argv[1]], input="\n".join(cases) + "\n", capture_output=True, text=True).stdout.split("\n")
fails = 0
for c, got in zip(cases, p):
    try:
        a = ipaddress.ip_address(c.split("%")[0] if ":" in c else c)
        want = str(a)
        if isinstance(a, ipaddress.IPv6Address) and a.ipv4_mapped: want = "::ffff:" + str(a.ipv4_mapped)
    except ValueError:
        want = "invalid"
    if got != want:
        fails += 1
        if fails < 15: print("FAIL %r: got %r want %r" % (c, got, want))
print("inet: %d addresses checked, %d failures" % (len(cases), fails))
sys.exit(1 if fails else 0)
