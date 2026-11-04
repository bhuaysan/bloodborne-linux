"""NIDs: the hashes PS4 ELFs use to import and export symbols instead of their names.

NID = SHA-1(name + suffix). The first 8 bytes of the digest are read as a little-endian 64-bit
number, written out big-endian and base64-encoded with '-' in place of '/'. The '=' padding is
dropped, which leaves 11 characters. The suffix comes from the psdevwiki Keys page. The byte order
and the encoding are our own findings, checked against libc.prx (see docs/formats/nid.md).
"""

from __future__ import annotations

import base64
import hashlib

NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
NID_LENGTH = 11
NID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"


def nid(name: str | bytes) -> str:
    """The NID of a symbol name. A str must be ASCII, like every PS4 symbol name we know."""
    raw = name.encode("ascii") if isinstance(name, str) else name
    digest = hashlib.sha1(raw + NID_SUFFIX).digest()
    text = base64.b64encode(digest[7::-1]).decode("ascii")  # bytes 7..0, i.e. little-endian
    return text.rstrip("=").replace("/", "-")


def nid_digest(text: str) -> bytes:
    """The inverse of nid(): the first 8 bytes of the SHA-1 digest that give this NID."""
    return base64.b64decode(text.replace("-", "/") + "=")[::-1]


def is_nid(text: str) -> bool:
    return len(text) == NID_LENGTH and all(c in NID_ALPHABET for c in text)
