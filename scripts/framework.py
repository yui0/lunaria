#!/usr/bin/env python3
"""Emit framework.h from a local AOSP core/res directory.

Usage: python3 scripts/framework.py AOSP_CORE_RES > src/dvm/framework.h
"""
import pathlib
import subprocess
import sys

scripts = pathlib.Path(__file__).resolve().parent
print('#ifndef LUNA_FRAMEWORK_H\n#define LUNA_FRAMEWORK_H\n')
for kind in ('strings', 'colors'):
    output = subprocess.check_output(
        [sys.executable, str(scripts / ('framework-' + kind + '.py')), sys.argv[1]],
        text=True,
    )
    guard = 'LUNA_FRAMEWORK_' + kind.upper() + '_H'
    output = output.replace('#ifndef ' + guard + '\n#define ' + guard + '\n', '')
    end = output.rfind('#endif')
    if end < 0:
        raise ValueError('missing header guard in ' + kind)
    print((output[:end] + output[end + len('#endif'):]).strip() + '\n')
print('#endif /* LUNA_FRAMEWORK_H */')
