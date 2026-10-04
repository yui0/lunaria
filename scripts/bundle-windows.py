#!/usr/bin/env python3
"""Collect the complete PE import closure beside lunaria.exe.

WINDOWS_DLL_DIRS is a path-separated list of additional DLL directories.
OBJDUMP may select a cross objdump. System DLLs remain provided by Windows;
every other missing import is an error, rather than a broken distribution.
"""
# SPDX-License-Identifier: MPL-2.0
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Windows components only. MinGW, OpenSSL, GLFW and ANGLE are bundled.
SYSTEM_DLLS = frozenset("""
advapi32.dll avrt.dll bcrypt.dll cabinet.dll cfgmgr32.dll comctl32.dll
comdlg32.dll crypt32.dll cryptbase.dll d3d9.dll d3d11.dll d3d12.dll
dcomp.dll dbghelp.dll dnsapi.dll dsound.dll dwmapi.dll dxgi.dll gdi32.dll
hid.dll imm32.dll iphlpapi.dll kernel32.dll kernelbase.dll mf.dll
mfplat.dll mfreadwrite.dll mfuuid.dll mmdevapi.dll mpr.dll msimg32.dll
msvcrt.dll ncrypt.dll normaliz.dll ntdll.dll ole32.dll oleaut32.dll
opengl32.dll powrprof.dll propsys.dll psapi.dll rpcrt4.dll secur32.dll
setupapi.dll shcore.dll shell32.dll shlwapi.dll ucrtbase.dll user32.dll
userenv.dll usp10.dll uxtheme.dll version.dll winhttp.dll wininet.dll
winmm.dll winspool.drv wintrust.dll wldap32.dll ws2_32.dll wtsapi32.dll
""".split())


def bundle(stage):
    objdump = os.environ.get("OBJDUMP") or shutil.which("x86_64-w64-mingw32-objdump") or "objdump"
    here = Path(__file__).resolve().parent.parent
    directories = [stage, stage / "runtime", here / ".deps/windows/bin"]
    # Native MSYS2 Python sees Windows paths, so /ucrt64 is not its install
    # prefix. Its own executable (or GCC) locates the actual toolchain bin.
    directories.append(Path(sys.executable).resolve().parent)
    compiler = shutil.which("gcc")
    if compiler:
        directories.append(Path(compiler).resolve().parent)
    directories += [Path(p) for p in os.environ.get("WINDOWS_DLL_DIRS", "").split(os.pathsep) if p]
    directories += [Path(p) for p in (
        "/mingw64/bin", "/ucrt64/bin", "/usr/x86_64-w64-mingw32/sys-root/mingw/bin")]
    index = {}
    for directory in directories:
        if directory.is_dir():
            for path in directory.iterdir():
                if path.is_file() and path.suffix.lower() == ".dll":
                    index.setdefault(path.name.lower(), path)
    queue = [stage / "lunaria.exe"]
    queue += sorted((stage / "runtime").glob("*.dll"))
    seen = set()
    while queue:
        binary = queue.pop()
        if binary.resolve() in seen:
            continue
        seen.add(binary.resolve())
        result = subprocess.run([objdump, "-p", str(binary)], check=True,
                                text=True, stdout=subprocess.PIPE)
        if "file format pei-x86-64" not in result.stdout:
            raise RuntimeError(f"{binary}: expected a Windows x86_64 PE binary")
        for name in re.findall(r"DLL Name:\s*(\S+)", result.stdout):
            key = name.lower()
            if key in SYSTEM_DLLS or key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            source = index.get(key)
            if source is None:
                raise RuntimeError(f"{binary.name}: missing {name}; set WINDOWS_DLL_DIRS")
            target = stage / name
            if source.resolve() != target.resolve():
                if target.exists():
                    raise RuntimeError(f"conflicting DLL copies: {target} and {source}")
                shutil.copy2(source, target)
            index[key] = target
            queue.append(target)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: bundle-windows.py <stage-dir>")
    try:
        bundle(Path(sys.argv[1]).resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f"bundle-windows: {error}")
