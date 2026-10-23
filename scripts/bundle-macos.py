#!/usr/bin/env python3
"""Bundle the Mach-O dependency closure and use relocatable @rpath names."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
stage=Path(sys.argv[1]).resolve()
here=Path(__file__).resolve().parents[1]
libdir=stage/'lib';libdir.mkdir(exist_ok=True)
search=[stage/'runtime',libdir,here/'runtime',here/'.deps/lib',Path(os.environ.get('MAC_OPENSSL','/opt/homebrew/opt/openssl@3'))/'lib']
browser=stage/'luna-browser'/'luna-browser'
queue=[(stage/'lunaria',here/'lunaria')]
# luna-browser is the WebView engine used when no Chrome is installed.  It is
# an executable one directory below the bundle root, so ../lib and ../runtime
# are where its bundled libraries are.
if browser.is_file():queue.append((browser,here/'luna-browser'/'luna-browser'))
queue.extend((p,here/'runtime'/p.name) for p in (stage/'runtime').iterdir() if p.is_file())
# ANGLE loads its Vulkan driver through a manifest, so otool's dependency
# closure cannot discover it. Ship the driver and its own dependency closure.
driver = here/'.deps/lib/libMoltenVK.dylib'
manifest = here/'.deps/lib/moltenvk_icd.json'
if not driver.is_file() or not manifest.is_file():
    sys.exit('bundle-macos: missing MoltenVK driver; run make macos-deps')
shutil.copy2(driver, libdir/driver.name)
queue.append((libdir/driver.name, driver))
driver_info = json.loads(manifest.read_text())
driver_info['ICD']['library_path'] = './' + driver.name
(libdir/manifest.name).write_text(json.dumps(driver_info) + '\n')
seen=set()
def install_id(path):
    # A dylib lists its own install name among its load commands (the first
    # line of `otool -L`); it is not something to be found and bundled.
    result=subprocess.run(['otool','-D',str(path)],check=True,text=True,stdout=subprocess.PIPE)
    lines=result.stdout.splitlines()
    return lines[1].strip() if len(lines)>1 else None
def dependencies(path):
    result=subprocess.run(['otool','-L',str(path)],check=True,text=True,stdout=subprocess.PIPE)
    own=install_id(path) if Path(path) not in (stage/'lunaria',browser) else None
    # Universal dylibs print a heading for each architecture. Only load-command
    # lines carry a compatibility version; headings are not dependencies.
    return [name for name in (line.strip().split(' (compatibility version',1)[0]
            for line in result.stdout.splitlines() if ' (compatibility version' in line)
            if name!=own]
def resolve(name,source):
    if name.startswith('@loader_path/'):
        candidates=[source.parent/name[len('@loader_path/'):]]
    elif name.startswith('@executable_path/'):
        candidates=[here/name[len('@executable_path/'):]]
    elif name.startswith('@rpath/'):
        candidates=[p/name[len('@rpath/'):] for p in search]
    else:candidates=[Path(name)]
    return next((p.resolve() for p in candidates if p.is_file()),None)
while queue:
    dest,source=queue.pop(0)
    if dest in seen:continue
    seen.add(dest)
    needs_rpath=False
    for name in dependencies(dest):
        if name.startswith(('/usr/lib/','/System/')):continue
        if name in ('@rpath/'+dest.name,str(source),str(dest)):continue  # own dylib ID
        needs_rpath=True
        original=resolve(name,source)
        if original is None:sys.exit(f'bundle-macos: missing dependency {name} (from {source})')
        target=(stage/'runtime'/original.name) if (stage/'runtime'/original.name).is_file() else libdir/original.name
        if not target.exists():shutil.copy2(original,target)
        target.chmod(target.stat().st_mode|0o200)
        subprocess.run(['install_name_tool','-change',name,'@rpath/'+target.name,str(dest)],check=True)
        queue.append((target,original))
    if dest==browser and needs_rpath:
        for relative in ('@loader_path/../lib','@loader_path/../runtime'):
            result=subprocess.run(['install_name_tool','-add_rpath',relative,str(dest)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if result.returncode and b'duplicate' not in result.stderr.lower():sys.exit(result.stderr.decode())
    if dest not in (stage/'lunaria',browser):
        subprocess.run(['install_name_tool','-id','@rpath/'+dest.name,str(dest)],check=True)
        # A dylib may need another bundled dylib before main() is entered.
        # System-only dylibs need no search paths; prebuilt universal drivers
        # can have insufficient load-command padding for unnecessary rpaths.
        for relative in (('@loader_path','@loader_path/../lib','@loader_path/../runtime') if needs_rpath else ()):
            result=subprocess.run(['install_name_tool','-add_rpath',relative,str(dest)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if result.returncode and b'duplicate' not in result.stderr.lower():sys.exit(result.stderr.decode())

# Rewriting Mach-O load commands invalidates existing ARM64 ad-hoc signatures.
for dest in seen:
    subprocess.run(['codesign','--force','--sign','-',str(dest)],check=True)
