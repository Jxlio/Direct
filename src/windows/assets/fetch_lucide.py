"""Fetch the selected upstream SVG sources and their license at a pinned revision."""
from pathlib import Path
from urllib.request import urlopen

revision = "c29c276f16590cf017126671535ad8c792be64d8"
names = ["message-circle", "users", "phone", "phone-off", "mic", "mic-off", "send", "monitor", "calendar-days", "gamepad-2", "settings"]
target = Path(__file__).with_name("lucide")
target.mkdir(exist_ok=True)
base = f"https://raw.githubusercontent.com/lucide-icons/lucide/{revision}/"
for name in names:
    (target / f"{name}.svg").write_bytes(urlopen(base + f"icons/{name}.svg").read())
(target / "LICENSE").write_bytes(urlopen(base + "LICENSE").read())
(target / "REVISION.txt").write_text(revision + "\n", encoding="utf-8")
print(f"Downloaded {len(names)} Lucide SVGs at {revision}.")
