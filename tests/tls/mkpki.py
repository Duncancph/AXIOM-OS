# Generates a throwaway test PKI (good and deliberately broken certificates) for tests/tls/matrix.sh.
import datetime
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec
import ipaddress
now = datetime.datetime.now(datetime.timezone.utc)
def name(cn): return x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, "DuncOS Test"), x509.NameAttribute(NameOID.COMMON_NAME, cn)])
def save(fn, obj): open(fn, "wb").write(obj.public_bytes(serialization.Encoding.PEM))
def savekey(fn, k): open(fn, "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
def cert(subject_cn, key, issuer_cn, ikey, h, ca=False, pathlen=None, sans=None, nb=None, na=None, eku=ExtendedKeyUsageOID.SERVER_AUTH):
    b = (x509.CertificateBuilder().subject_name(name(subject_cn)).issuer_name(name(issuer_cn)).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(nb or now - datetime.timedelta(days=1)).not_valid_after(na or now + datetime.timedelta(days=365)))
    b = b.add_extension(x509.BasicConstraints(ca=ca, path_length=pathlen), critical=True)
    if ca: b = b.add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True)
    else:
        b = b.add_extension(x509.KeyUsage(True, False, True, False, False, False, False, False, False), critical=True)
        b = b.add_extension(x509.ExtendedKeyUsage([eku]), critical=False)
    if sans: b = b.add_extension(x509.SubjectAlternativeName(sans), critical=False)
    return b.sign(ikey, h)
SANS = [x509.DNSName("duncos.test"), x509.DNSName("*.duncos.test"), x509.IPAddress(ipaddress.ip_address("127.0.0.1")), x509.IPAddress(ipaddress.ip_address("2001:db8:2::80"))]
# RSA chain
rroot = rsa.generate_private_key(65537, 4096); rint = rsa.generate_private_key(65537, 2048); rleaf = rsa.generate_private_key(65537, 2048)
save("rsa_root.pem", cert("Test RSA Root", rroot, "Test RSA Root", rroot, hashes.SHA256(), ca=True))
save("rsa_int.pem", cert("Test RSA Intermediate", rint, "Test RSA Root", rroot, hashes.SHA384(), ca=True, pathlen=0))
save("rsa_leaf.pem", cert("duncos.test", rleaf, "Test RSA Intermediate", rint, hashes.SHA256(), sans=SANS)); savekey("rsa_leaf.key", rleaf)
# EC chain (P-384 root and intermediate, P-256 and P-384 leaves)
eroot = ec.generate_private_key(ec.SECP384R1()); eint = ec.generate_private_key(ec.SECP384R1())
eleaf = ec.generate_private_key(ec.SECP256R1()); eleaf384 = ec.generate_private_key(ec.SECP384R1())
save("ec_root.pem", cert("Test EC Root", eroot, "Test EC Root", eroot, hashes.SHA384(), ca=True))
save("ec_int.pem", cert("Test EC Intermediate", eint, "Test EC Root", eroot, hashes.SHA384(), ca=True))
save("ec_leaf.pem", cert("duncos.test", eleaf, "Test EC Intermediate", eint, hashes.SHA256(), sans=SANS)); savekey("ec_leaf.key", eleaf)
save("ec384_leaf.pem", cert("duncos.test", eleaf384, "Test EC Intermediate", eint, hashes.SHA384(), sans=SANS)); savekey("ec384_leaf.key", eleaf384)
# bad ones
k = rsa.generate_private_key(65537, 2048)
save("expired_leaf.pem", cert("duncos.test", k, "Test RSA Intermediate", rint, hashes.SHA256(), sans=SANS, nb=now - datetime.timedelta(days=400), na=now - datetime.timedelta(days=30))); savekey("expired_leaf.key", k)
save("other_leaf.pem", cert("other.test", k, "Test RSA Intermediate", rint, hashes.SHA256(), sans=[x509.DNSName("other.test")])); savekey("other_leaf.key", k)
save("self_leaf.pem", cert("duncos.test", k, "duncos.test", k, hashes.SHA256(), sans=SANS)); savekey("self_leaf.key", k)
save("client_leaf.pem", cert("duncos.test", k, "Test RSA Intermediate", rint, hashes.SHA256(), sans=SANS, eku=ExtendedKeyUsageOID.CLIENT_AUTH)); savekey("client_leaf.key", k)
open("ca.pem", "w").write(open("rsa_root.pem").read() + open("ec_root.pem").read())
