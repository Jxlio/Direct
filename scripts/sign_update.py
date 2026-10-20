"""Create the three assets consumed by Direct's native updater."""
import argparse
import hashlib
import re
import shutil
import struct
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization

p = argparse.ArgumentParser()
p.add_argument("--version", required=True)
p.add_argument("--exe", type=Path, required=True)
p.add_argument("--key", type=Path, required=True)
p.add_argument("--output", type=Path, required=True)
a = p.parse_args()
if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", a.version):
    p.error("Expected a stable numeric version, e.g. 0.6.0")
data = a.exe.read_bytes()
if not 0 < len(data) <= 32*1024*1024 or data[:2] != b"MZ":
    p.error("Expected a Windows executable smaller than 32 MiB")
if len(data)<64:
    p.error("Invalid executable")
pe = struct.unpack_from("<I",data,0x3c)[0]
if pe+26>len(data) or data[pe:pe+4]!=b"PE\0\0" or struct.unpack_from("<H",data,pe+4)[0]!=0x8664:
    p.error("Expected an x64 Windows executable")
manifest = f"DIRECT-UPDATE-1\n{a.version}\nwin-x64\n{len(data)}\n{hashlib.sha256(data).hexdigest()}\n".encode("ascii")
key = serialization.load_pem_private_key(a.key.read_bytes(), password=None)
public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode("ascii")
header = (Path(__file__).resolve().parents[1]/"src/update_key.h").read_text(encoding="utf-8")
import json
if json.dumps(public) not in header:
    p.error("Signing key does not match the application's pinned public key")
a.output.mkdir(parents=True, exist_ok=True)
(a.output/"Direct-win-x64.manifest").write_bytes(manifest)
(a.output/"Direct-win-x64.sig").write_bytes(key.sign(manifest, ec.ECDSA(hashes.SHA256())))
shutil.copyfile(a.exe, a.output/"Direct-win-x64.exe")
print("Signed update assets ready.")
