#!/usr/bin/env python3
"""Generate Android 12's two independent MIME maps from committed AOSP inputs."""
from pathlib import Path
import json
root = Path(__file__).resolve().parents[1]
mime_to_ext, ext_to_mime = {}, {}
def put(mapping, spec, value):
    keep = spec.startswith('?')
    key = spec.lstrip('?').lower()
    if not keep or key not in mapping:
        mapping[key] = value.lstrip('?').lower()
for name in ['debian.mime.types', 'android.mime.types', 'vendor.mime.types']:
    for line in (root / 'scripts/mime-data' / name).read_text().splitlines():
        row = line.split('#', 1)[0].split()
        if len(row) < 2:
            continue
        if name == 'vendor.mime.types':
            row = ['?' + field.lstrip('?') for field in row]
        mime, *extensions = row
        put(mime_to_ext, mime, extensions[0])
        for extension in extensions:
            put(ext_to_mime, extension, mime)
lines = ['/* Generated Android 12.0.0_r1 MIME data. AOSP Apache-2.0 and public domain.',
         ' * Sources and licenses: scripts/mime-data/README.md. */',
         'struct rt_mime_entry { const char *key, *value; };']
for name, mapping in [('rt_mime_to_extension', mime_to_ext), ('rt_extension_to_mime', ext_to_mime)]:
    lines.append('static const struct rt_mime_entry ' + name + '[] = {')
    for key, value in sorted(mapping.items()):
        assert key.isascii() and value.isascii()
        lines.append('   {' + json.dumps(key) + ', ' + json.dumps(value) + '},')
    lines.append('};')
(root / 'src/dvm/mime_data.h').write_text('\n'.join(lines) + '\n')
