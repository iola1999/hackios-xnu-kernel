"""Import the factory Wi-Fi address saved locally with the phone's firmware."""
import re
from pathlib import Path


def normalize_factory_mac(text: str) -> str:
    address = text.strip().lower()
    if not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", address):
        raise ValueError("expected one colon-separated factory Wi-Fi address")
    if int(address[:2], 16) & 1 or address in {"00:00:00:00:00:00", "02:00:00:00:00:00"}:
        raise ValueError("invalid or unavailable factory Wi-Fi address")
    return address


def read_factory_mac(path: Path) -> str:
    try:
        return normalize_factory_mac(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, ValueError):
        raise ValueError("could not read one factory Wi-Fi address from the local file") from None
