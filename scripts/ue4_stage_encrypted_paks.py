#!/usr/bin/env python3
# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# Stage selected assets out of UE4 AES-encrypted pak indexes so the guest can
# open() them as loose files.  Emulator-side staging only — no APK/guest patch.
#
# This title stores the AES-256 key as 32 ASCII hex digits and encrypts the pak
# *index* with AES-256-ECB (IV unused).  Per-entry payloads are zlib (and
# optionally AES-ECB) with RelativeChunkOffsets.

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

# AES-256-ECB, from whichever backend this host actually has.  Staging is not
# optional — without it the engine never sees the .uproject and dies in PreInit
# — so a missing or half-installed `cryptography` must not take the whole run
# down.  (A broken install raises pyo3's PanicException, which derives from
# BaseException and sails straight past `except ImportError`.)  Order: the
# extension module, then libcrypto through ctypes (lunaria already links it),
# then a self-contained implementation that always works.


def _backend_cryptography():
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def dec(data: bytes, key: bytes) -> bytes:
        d = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
        return d.update(data) + d.finalize()

    dec(b"\0" * 16, b"\0" * 32)
    return dec


def _backend_libcrypto():
    import ctypes
    import ctypes.util

    name = ctypes.util.find_library("crypto") or "libcrypto.so.3"
    lib = ctypes.CDLL(name)
    lib.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
    for sym in ("EVP_aes_128_ecb", "EVP_aes_192_ecb", "EVP_aes_256_ecb"):
        getattr(lib, sym).restype = ctypes.c_void_p

    def dec(data: bytes, key: bytes) -> bytes:
        cipher = getattr(lib, "EVP_aes_%d_ecb" % (len(key) * 8))()
        ctx = lib.EVP_CIPHER_CTX_new()
        if not ctx:
            raise RuntimeError("EVP_CIPHER_CTX_new failed")
        try:
            if lib.EVP_DecryptInit_ex(ctypes.c_void_p(ctx), ctypes.c_void_p(cipher),
                                      None, key, None) != 1:
                raise RuntimeError("EVP_DecryptInit_ex failed")
            lib.EVP_CIPHER_CTX_set_padding(ctypes.c_void_p(ctx), 0)
            out = ctypes.create_string_buffer(len(data) + 16)
            n = ctypes.c_int(0)
            if lib.EVP_DecryptUpdate(ctypes.c_void_p(ctx), out, ctypes.byref(n),
                                     data, len(data)) != 1:
                raise RuntimeError("EVP_DecryptUpdate failed")
            return out.raw[: n.value]
        finally:
            lib.EVP_CIPHER_CTX_free(ctypes.c_void_p(ctx))

    dec(b"\0" * 16, b"\0" * 32)
    return dec


def _backend_pure_python():
    """Table-driven AES decryption — no extension modules, no shared libraries."""
    sbox = bytearray(256)
    inv_sbox = bytearray(256)
    p = q = 1
    while True:  # generate the S-box from the AES affine transform over GF(2^8)
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        v = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) \
              ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (v ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    for i, s in enumerate(sbox):
        inv_sbox[s] = i

    def xt(a: int, b: int) -> int:  # GF(2^8) multiply
        r = 0
        for _ in range(8):
            if b & 1:
                r ^= a
            hi = a & 0x80
            a = (a << 1) & 0xFF
            if hi:
                a ^= 0x1B
            b >>= 1
        return r

    # Equivalent-inverse-cipher tables: tdN is td0 rotated right by 8*N bits.
    td0 = [0] * 256
    for i in range(256):
        s = inv_sbox[i]
        td0[i] = (xt(s, 0x0E) << 24) | (xt(s, 0x09) << 16) | (xt(s, 0x0D) << 8) | xt(s, 0x0B)
    def ror(w: int, n: int) -> int:
        return ((w >> n) | (w << (32 - n))) & 0xFFFFFFFF
    td1 = [ror(w, 8) for w in td0]
    td2 = [ror(w, 16) for w in td0]
    td3 = [ror(w, 24) for w in td0]

    rcon = [0x01]
    for _ in range(13):
        rcon.append(xt(rcon[-1], 2))

    def expand(key: bytes):
        nk = len(key) // 4
        nr = nk + 6
        w = list(struct.unpack(">%dI" % nk, key))
        for i in range(nk, 4 * (nr + 1)):
            t = w[i - 1]
            if i % nk == 0:
                t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
                t = (sbox[(t >> 24) & 0xFF] << 24) | (sbox[(t >> 16) & 0xFF] << 16) \
                    | (sbox[(t >> 8) & 0xFF] << 8) | sbox[t & 0xFF]
                t ^= rcon[i // nk - 1] << 24
            elif nk > 6 and i % nk == 4:
                t = (sbox[(t >> 24) & 0xFF] << 24) | (sbox[(t >> 16) & 0xFF] << 16) \
                    | (sbox[(t >> 8) & 0xFF] << 8) | sbox[t & 0xFF]
            w.append(w[i - nk] ^ t)
        # Decryption round keys: reversed, with InvMixColumns folded in so the
        # round loop can use the T-tables above.
        dk = []
        for r in range(nr + 1):
            src = w[4 * (nr - r): 4 * (nr - r) + 4]
            if 0 < r < nr:
                src = [
                    td0[sbox[(x >> 24) & 0xFF]] ^ td1[sbox[(x >> 16) & 0xFF]]
                    ^ td2[sbox[(x >> 8) & 0xFF]] ^ td3[sbox[x & 0xFF]]
                    for x in src
                ]
            dk.append(src)
        return dk, nr

    cache: dict[bytes, tuple] = {}

    def dec(data: bytes, key: bytes) -> bytes:
        sched = cache.get(key)
        if sched is None:
            sched = cache[key] = expand(key)
        dk, nr = sched
        out = bytearray(len(data))
        pack = struct.pack_into
        for off in range(0, len(data), 16):
            s0, s1, s2, s3 = struct.unpack_from(">4I", data, off)
            k = dk[0]
            s0 ^= k[0]; s1 ^= k[1]; s2 ^= k[2]; s3 ^= k[3]
            for r in range(1, nr):
                k = dk[r]
                t0 = (td0[(s0 >> 24) & 0xFF] ^ td1[(s3 >> 16) & 0xFF]
                      ^ td2[(s2 >> 8) & 0xFF] ^ td3[s1 & 0xFF] ^ k[0])
                t1 = (td0[(s1 >> 24) & 0xFF] ^ td1[(s0 >> 16) & 0xFF]
                      ^ td2[(s3 >> 8) & 0xFF] ^ td3[s2 & 0xFF] ^ k[1])
                t2 = (td0[(s2 >> 24) & 0xFF] ^ td1[(s1 >> 16) & 0xFF]
                      ^ td2[(s0 >> 8) & 0xFF] ^ td3[s3 & 0xFF] ^ k[2])
                t3 = (td0[(s3 >> 24) & 0xFF] ^ td1[(s2 >> 16) & 0xFF]
                      ^ td2[(s1 >> 8) & 0xFF] ^ td3[s0 & 0xFF] ^ k[3])
                s0, s1, s2, s3 = t0, t1, t2, t3
            k = dk[nr]
            pack(
                ">4I", out, off,
                ((inv_sbox[(s0 >> 24) & 0xFF] << 24) | (inv_sbox[(s3 >> 16) & 0xFF] << 16)
                 | (inv_sbox[(s2 >> 8) & 0xFF] << 8) | inv_sbox[s1 & 0xFF]) ^ k[0],
                ((inv_sbox[(s1 >> 24) & 0xFF] << 24) | (inv_sbox[(s0 >> 16) & 0xFF] << 16)
                 | (inv_sbox[(s3 >> 8) & 0xFF] << 8) | inv_sbox[s2 & 0xFF]) ^ k[1],
                ((inv_sbox[(s2 >> 24) & 0xFF] << 24) | (inv_sbox[(s1 >> 16) & 0xFF] << 16)
                 | (inv_sbox[(s0 >> 8) & 0xFF] << 8) | inv_sbox[s3 & 0xFF]) ^ k[2],
                ((inv_sbox[(s3 >> 24) & 0xFF] << 24) | (inv_sbox[(s2 >> 16) & 0xFF] << 16)
                 | (inv_sbox[(s1 >> 8) & 0xFF] << 8) | inv_sbox[s0 & 0xFF]) ^ k[3],
            )
        return bytes(out)

    return dec


def _pick_aes_backend():
    for name, make in (("cryptography", _backend_cryptography),
                       ("libcrypto", _backend_libcrypto),
                       ("builtin", _backend_pure_python)):
        try:
            dec = make()
        except BaseException as ex:  # noqa: BLE001 — pyo3 panics are not Exceptions
            print(f"ue4_stage_encrypted_paks: AES backend {name} unavailable: "
                  f"{type(ex).__name__}: {ex}", file=sys.stderr)
            continue
        if name != "cryptography":
            print(f"ue4_stage_encrypted_paks: using {name} AES backend", file=sys.stderr)
        return dec
    raise SystemExit("ue4_stage_encrypted_paks: no usable AES backend")


_aes_dec = None


def aes_ecb_dec(data: bytes, key: bytes) -> bytes:
    global _aes_dec
    n = len(data) - len(data) % 16
    if n <= 0:
        return b""
    if _aes_dec is None:
        _aes_dec = _pick_aes_backend()
    return _aes_dec(data[:n], key)


def elf64_vaddr_mapper(so: bytes):
    """Map a link-time virtual address to its offset in the ELF file.

    Symbol values and adrp/add results are virtual addresses; only for a
    segment whose p_offset happens to equal p_vaddr does indexing the file
    with them work.  libUE4.so aligns its executable segment to 0x4000 and
    its data segments to 0x8000, so the two differ and every read lands in
    the wrong function.  Walk PT_LOAD instead.
    """
    if len(so) < 64 or so[:4] != b"\x7fELF" or so[4] != 2:
        return None
    e_phoff = struct.unpack_from("<Q", so, 0x20)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", so, 0x36)
    loads = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        if o + 56 > len(so):
            break
        p_type = struct.unpack_from("<I", so, o)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr, _p_paddr, p_filesz = struct.unpack_from("<QQQQ", so, o + 8)
        loads.append((p_vaddr, p_offset, p_filesz))

    def to_off(vaddr: int, size: int = 1):
        for seg_vaddr, seg_off, seg_filesz in loads:
            if seg_vaddr <= vaddr and vaddr + size <= seg_vaddr + seg_filesz:
                return vaddr - seg_vaddr + seg_off
        return None

    return to_off if loads else None


def key_from_ue4_so(so_path: Path) -> bytes | None:
    """Read the 32-byte pak key from FEncryptionKeyRegistration::Callback."""
    import subprocess

    try:
        out = subprocess.check_output(
            ["readelf", "-Ws", str(so_path)], text=True, stderr=subprocess.DEVNULL
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    addr = None
    for line in out.splitlines():
        if "FEncryptionKeyRegistration8CallbackEPh" in line:
            parts = line.split()
            for p in parts:
                if all(c in "0123456789abcdefABCDEF" for c in p) and len(p) >= 4:
                    try:
                        addr = int(p, 16)
                        break
                    except ValueError:
                        pass
            if addr is not None:
                break
    if addr is None:
        return None
    so = so_path.read_bytes()
    to_off = elf64_vaddr_mapper(so)
    if to_off is None:
        return None
    # Callback: adrp x8, page; add x8, x8, #imm; ldp q0,q1,[x8]; stp; ret
    code_off = to_off(addr, 8)
    if code_off is None:
        return None
    w0, w1 = struct.unpack_from("<II", so, code_off)
    # adrp: immlo in [30:29], immhi in [23:5], Rd in [4:0]
    if (w0 & 0x9F000000) != 0x90000000:
        return None
    rd = w0 & 0x1F
    immlo = (w0 >> 29) & 0x3
    immhi = (w0 >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= 1 << 21
    page = (addr & ~0xFFF) + (imm << 12)
    # add x8, x8, #imm12
    if (w1 & 0xFFC00000) != 0x91000000:
        return None
    imm12 = (w1 >> 10) & 0xFFF
    key_off = to_off(page + imm12, 32)
    if key_off is None:
        return None
    key = so[key_off : key_off + 32]
    if not all(48 <= b <= 57 or 97 <= b <= 102 or 65 <= b <= 70 for b in key):
        # Still accept any 32-byte key; CryptoKeys often stores ASCII hex.
        pass
    return bytes(key)


def read_fstring(data: bytes, off: int):
    n = struct.unpack_from("<i", data, off)[0]
    if n == 0:
        return "", off + 4
    if n < 0:
        n = -n
        s = data[off + 4 : off + 4 + n * 2].decode("utf-16-le", "replace").rstrip("\0")
        return s, off + 4 + n * 2
    s = data[off + 4 : off + 4 + n].decode("utf-8", "replace").rstrip("\0")
    return s, off + 4 + n


def parse_index(pt: bytes):
    mount, off = read_fstring(pt, 0)
    num = struct.unpack_from("<i", pt, off)[0]
    off += 4
    entries = []
    for _ in range(num):
        name, off = read_fstring(pt, off)
        offset, size, usize, cm = struct.unpack_from("<qqqi", pt, off)
        off += 28
        off += 20  # hash
        blocks = []
        enc = 0
        if cm != 0:
            nb = struct.unpack_from("<i", pt, off)[0]
            off += 4
            if not (0 <= nb < 100000):
                raise ValueError(f"bad compression block count {nb} for {name}")
            for _b in range(nb):
                a, b = struct.unpack_from("<qq", pt, off)
                off += 16
                blocks.append((a, b))
        # FPakEntry::Serialize writes Flags (bit 0 = encrypted) *before*
        # CompressionBlockSize, for compressed and uncompressed entries alike.
        enc = pt[off] & 0x01
        off += 5
        entries.append(
            {
                "name": name,
                "offset": offset,
                "size": size,
                "usize": usize,
                "cm": cm,
                "enc": enc,
                "blocks": blocks,
            }
        )
    return mount, entries


def extract_entry(pak: bytes, e: dict, key: bytes) -> bytes:
    base = e["offset"]
    if e["cm"] == 0:
        hdr = 28 + 20 + 5
        raw = pak[base + hdr : base + hdr + e["size"]]
        if e["enc"]:
            al = (e["size"] + 15) & ~15
            return aes_ecb_dec(pak[base + hdr : base + hdr + al], key)[: e["size"]]
        return raw
    out = bytearray()
    for start, end in e["blocks"]:
        abs_s = base + start
        clen = end - start
        if e["enc"]:
            al = (clen + 15) & ~15
            chunk = aes_ecb_dec(pak[abs_s : abs_s + al], key)[:clen]
            out += zlib.decompress(chunk)
        else:
            chunk = pak[abs_s : abs_s + clen]
            try:
                out += zlib.decompress(chunk)
            except zlib.error:
                # Some Android cooked paks leave Encrypted=0 in the index while
                # the compression blocks are still AES-ECB (Config/*.ini here).
                al = (clen + 15) & ~15
                chunk = aes_ecb_dec(pak[abs_s : abs_s + al], key)[:clen]
                out += zlib.decompress(chunk)
    return bytes(out)


def want_entry(name: str) -> bool:
    # Early PreInit probes .uproject before FPakFile is mounted; without a
    # loose copy UE aborts ("Failed to open descriptor file").  The real
    # descriptor lives in the encrypted pak — stage that, never a fabricated
    # stub (other titles must not see a fake project file).
    # Config + ICU are also probed via AAssetManager before/alongside pak
    # mount; staging them lets the asset bridge and filesystem answer.
    # Login shell also soft-loads UI_Fixed / Movies/Login / GameBlueprint
    # before (or beside) FPak reads; CurlCertificates are needed for CDN
    # HTTPS when DownloadContent pulls the rest of the login UI.
    base = name.rsplit("/", 1)[-1]
    return (
        name.endswith(".uproject")
        or "ShaderArchive" in name
        or "GlobalShaderCache" in name
        or name.endswith("AssetRegistry.bin")
        or name.endswith(".umap")
        or (name.endswith(".uexp") and ("/Maps/" in name or base.startswith("Map_")))
        or "/Config/" in name
        or "Internationalization" in name
        or name.endswith(".pem")
        or "CurlCertificates" in name
        or "/UI_Fixed/" in name
        or "/Movies/" in name
        or "/GameBlueprint/" in name
        or ("/UI/Atlas/" in name and "LogIn" in name)
    )


PAK_MAGIC = 0x5A6F12E1


def parse_footer(pak: bytes):
    """Locate and read FPakInfo at the end of the pak.

    The footer is not a fixed 44 bytes: FPakInfo::Serialize writes an
    EncryptionKeyGuid (v7+), an encrypted-index flag (v4+), a frozen-index
    flag (v9) and 5*32 bytes of compression method names (v8+) around the
    core magic/version/index fields.  Reading a fixed 44-byte tail finds no
    magic on any modern pak and the whole file is skipped.  Derive the layout
    from the version instead, and let the version field validate the guess.
    """
    for ver in range(11, 0, -1):
        tail = 4 + 4 + 8 + 8 + 20  # magic, version, index offset/size, hash
        if ver == 9:
            tail += 1  # bIndexIsFrozen
        if ver >= 8:
            tail += 5 * 32  # CompressionMethods[MaxNumCompressionMethods]
        if tail > len(pak):
            continue
        pos = len(pak) - tail
        magic, file_ver, idx_off, idx_size = struct.unpack_from("<IIQQ", pak, pos)
        if magic != PAK_MAGIC or file_ver != ver:
            continue
        idx_hash = pak[pos + 24 : pos + 44]
        encrypted = True
        if ver >= 4:  # PakFile_Version_IndexEncryption
            if pos < 1:
                continue
            encrypted = pak[pos - 1] != 0
        return {
            "version": ver,
            "index_offset": idx_off,
            "index_size": idx_size,
            "index_hash": idx_hash,
            "encrypted_index": encrypted,
        }
    return None


def stage_pak(pak_path: Path, out_root: Path, key: bytes) -> int:
    pak = pak_path.read_bytes()
    info = parse_footer(pak)
    if info is None:
        print(f"  {pak_path.name}: no FPakInfo footer", file=sys.stderr)
        return 0
    ver, idx_off, idx_size = info["version"], info["index_offset"], info["index_size"]
    if idx_size <= 0 or idx_off + idx_size > len(pak):
        print(f"  {pak_path.name}: index out of range", file=sys.stderr)
        return 0
    if ver >= 10:
        # PathHashIndex/encoded entries — a different index format entirely.
        print(f"  {pak_path.name}: pak version {ver} index not supported yet",
              file=sys.stderr)
        return 0
    raw_idx = pak[idx_off : idx_off + idx_size]
    if info["encrypted_index"]:
        aligned = pak[idx_off : idx_off + ((idx_size + 15) & ~15)]
        pt = aes_ecb_dec(aligned, key)
    else:
        pt = raw_idx
    if len(pt) < idx_size or hashlib.sha1(pt[:idx_size]).digest() != info["index_hash"]:
        print(f"  {pak_path.name}: index hash mismatch (wrong AES key?)",
              file=sys.stderr)
        return 0
    mount, entries = parse_index(pt[:idx_size])
    prefix = mount[len("../../../") :] if mount.startswith("../../../") else mount
    n = 0
    for e in entries:
        if not want_entry(e["name"]):
            continue
        try:
            data = extract_entry(pak, e, key)
        except Exception as ex:
            print(f"  skip {e['name']}: {ex}", file=sys.stderr)
            continue
        # Entry names are usually Project/... or Engine/... (mount is ../../../).
        # Only prepend the mount leftover when the entry has no directory component.
        rel = e["name"].lstrip("/")
        if "/" not in rel and prefix:
            rel = prefix.rstrip("/") + "/" + rel
        dest = out_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists() and dest.stat().st_size == len(data):
            n += 1
            continue
        dest.write_bytes(data)
        n += 1
    return n


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--so", type=Path, help="libUE4.so for key discovery")
    ap.add_argument("--key", help="32-byte AES key as ASCII/hex string")
    ap.add_argument("--paks", type=Path, required=True, help="directory with *.pak")
    ap.add_argument("--out", type=Path, required=True, help="UE4Game/<Project> root")
    args = ap.parse_args()

    key = None
    if args.key:
        key = args.key.encode("ascii")
        if len(key) == 64:
            key = bytes.fromhex(args.key)
        elif len(key) != 32:
            print("ue4_stage_encrypted_paks: key must be 32 bytes or 64 hex", file=sys.stderr)
            return 1
    elif args.so and args.so.is_file():
        key = key_from_ue4_so(args.so)
    if not key or len(key) not in (16, 24, 32):
        print("ue4_stage_encrypted_paks: no pak AES key", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    total = 0
    for pak in sorted(args.paks.rglob("*.pak")):
        try:
            n = stage_pak(pak, args.out, key)
        except Exception as ex:
            print(f"ue4_stage_encrypted_paks: {pak.name}: {ex}", file=sys.stderr)
            continue
        if n:
            print(f"ue4_stage_encrypted_paks: {pak.name}: staged {n} assets")
            total += n
    if total:
        print(f"ue4_stage_encrypted_paks: {total} assets under {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
