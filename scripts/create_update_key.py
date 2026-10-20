"""Create/reuse the publisher key outside the repository; only export its public key."""
import argparse
import json
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization

p = argparse.ArgumentParser()
p.add_argument("--private-key", type=Path, required=True)
p.add_argument("--public-header", type=Path, required=True)
a = p.parse_args()
if a.private_key.exists():
    key = serialization.load_pem_private_key(a.private_key.read_bytes(), password=None)
else:
    a.private_key.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    a.private_key.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode("ascii")
a.public_header.write_text("/* Public verification key; the private key must never be committed. */\n#define DIRECT_UPDATE_PUBLIC_KEY " + json.dumps(public) + "\n", encoding="utf-8")
print("Publisher key ready. Only the public verification key was written to the project.")
