#!/usr/bin/env python3
"""Install unmodified framework resources from Google's Android SDK Platform 31.

Download: https://dl.google.com/android/repository/platform-31_r01.zip
Usage: python3 scripts/install-framework.py platform-31_r01.zip [output-directory]
"""
import hashlib
import io
from pathlib import Path
import sys
import zipfile

archive = Path(sys.argv[1])
target = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1] / 'runtime/framework-res'
payload = archive.read_bytes()
if hashlib.sha1(payload).hexdigest() != 'ca5bcaa565cb37e9d287051d6dd0e49a5426ec29':
    raise SystemExit('SDK Platform 31 archive checksum mismatch')
with zipfile.ZipFile(io.BytesIO(payload)) as sdk:
    jar = sdk.read('android-12/android.jar')
with zipfile.ZipFile(io.BytesIO(jar)) as framework:
    for item in framework.infolist():
        name = item.filename
        if item.is_dir() or not (name == 'resources.arsc' or name.startswith(('res/', 'NOTICES/'))):
            continue
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise SystemExit('Unsafe framework archive path')
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(framework.read(item))
(target / 'SOURCE.txt').write_text(
    'Android SDK Platform 31, revision 1\n'
    'https://dl.google.com/android/repository/platform-31_r01.zip\n'
    'SHA1 ca5bcaa565cb37e9d287051d6dd0e49a5426ec29\n'
    'resources.arsc, res/ and NOTICES/ copied without modification from android-12/android.jar\n')
print(target)
