"""Test-only signatures: never use the publisher key for publicly shipped fixtures."""
from pathlib import Path
import json
import hashlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

root = Path(__file__).resolve().parents[1]/"tests"
key = ec.generate_private_key(ec.SECP256R1())
public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode("ascii")
(root/"update_test_key.h").write_text("/* Test-only public key, never used in Direct.exe. */\n#define DIRECT_UPDATE_PUBLIC_KEY "+json.dumps(public)+"\n",encoding="utf-8")
payload = b"MZdirect-test-only-not-an-executable"
manifest = f"DIRECT-UPDATE-1\n9.8.7\nwin-x64\n{len(payload)}\n{hashlib.sha256(payload).hexdigest()}\n".encode()
cases = {"fixture_manifest": manifest, "fixture_signature": key.sign(manifest, ec.ECDSA(hashes.SHA256())), "fixture_payload": payload}
invalid = manifest.replace(b"win-x64", b"win-x86")
cases.update({"fixture_wrong_platform": invalid,"fixture_wrong_platform_sig":key.sign(invalid,ec.ECDSA(hashes.SHA256()))})
lines=["/* Public test fixtures. Their ephemeral private key was not saved. */"]
for name,data in cases.items():
    lines.append(f"static const unsigned char {name}[] = {{"+",".join(str(b) for b in data)+"};")
(root/"update_fixture.h").write_text("\n".join(lines)+"\n",encoding="utf-8")
