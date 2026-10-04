#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Download MSYS2 UCRT64 packages and their runtime dependency closure."""
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
cache = root / "packages"
cache.mkdir(exist_ok=True)
repo = "https://repo.msys2.org/mingw/ucrt64/"
prefix = "mingw-w64-ucrt-x86_64-"


def download(url, target):
    if target.is_file():
        return
    partial = target.with_name(target.name + ".part")
    subprocess.run(["curl", "-fL", "--connect-timeout", "20", "--retry", "3",
                    "-o", str(partial), url], check=True)
    partial.replace(target)


def run():
    # Repository metadata selects matching versions for the entire closure.
    with tempfile.TemporaryDirectory(dir=cache) as tmp:
        database = Path(tmp) / "ucrt64.db"
        download(repo + "ucrt64.db", database)
        subprocess.run(["tar", "xf", str(database), "-C", tmp], check=True)
        packages = {}
        for desc in Path(tmp).glob("*/desc"):
            fields = {}
            for section in desc.read_text().split("\n\n"):
                lines = section.strip().splitlines()
                if lines:
                    fields[lines[0].strip("%")] = lines[1:]
            packages[fields["NAME"][0]] = fields
        # GNU regex was removed from the current database but remains archived.
        # Lunaria uses its regex ABI, so retain this known UCRT package.
        packages.setdefault(prefix + "libgnurx", {
            "FILENAME": [prefix + "libgnurx-2.5.1-3-any.pkg.tar.zst"],
            "SHA256SUM": ["6b48bc9def347aaa70ebd06a35a5b3399182277ee5d57f9d9f8701d3acc01dc9"],
        })
        pending = [prefix + name for name in (
            "angleproject", "glfw", "openssl", "zlib", "libgnurx", "vulkan-headers")]
        seen = set()
        while pending:
            name = re.split(r"[<>=]", pending.pop())[0]
            if name in seen:
                continue
            seen.add(name)
            if name not in packages:
                raise RuntimeError(f"package missing from UCRT64 repository: {name}")
            info = packages[name]
            archive = cache / info["FILENAME"][0]
            download(repo + archive.name, archive)
            with archive.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if digest != info["SHA256SUM"][0]:
                archive.unlink()
                raise RuntimeError(f"checksum mismatch: {archive.name}; retry the download")
            print(f"windows-deps: extracting {name}", flush=True)
            entries = subprocess.check_output(["tar", "tf", str(archive)], text=True)
            if any(entry.startswith("ucrt64/") for entry in entries.splitlines()):
                subprocess.run(["tar", "xf", str(archive), "-C", str(root), "ucrt64"], check=True)
            pending.extend(info.get("DEPENDS", []))
    # Boost is used only for headers by Dynarmic; no Windows Boost DLLs needed.
    archive = cache / "boost_1_90_0.tar.bz2"
    download("https://archives.boost.io/release/1.90.0/source/" + archive.name, archive)
    subprocess.run(["tar", "xf", str(archive), "-C", str(root),
                    "--strip-components=1", "boost_1_90_0/boost"], check=True)
    (root / "include").mkdir(exist_ok=True)
    download("https://git.musl-libc.org/cgit/musl/plain/include/elf.h?h=v1.2.5",
             root / "include/elf.h")
    (root / ".ready").write_text("MSYS2 UCRT64 + Boost 1.90.0\n")


if __name__ == "__main__":
    try:
        run()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"windows-deps: {error}")
