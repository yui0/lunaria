#!/bin/sh
# Extracts the platform's libraries and dynamic linker from an Android system image into
# syslib-arm64/android/, where Lunaria installs them as the device's /apex tree.
#
# usage: extract-android-apex.sh SYSTEM_IMAGE [DEST]
#   SYSTEM_IMAGE  a raw ext4 `system` partition image (a dynamic-partition `super`
#                 image is unpacked first with lpunpack), optionally gzip-compressed
#   DEST          default: syslib-arm64/android
# needs: debugfs (e2fsprogs), unzip
set -eu
[ $# -ge 1 ] || { echo "usage: $0 SYSTEM_IMAGE [DEST]" >&2; exit 2; }
img=$1
dest=${2:-$(dirname "$0")/../syslib-arm64/android}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
case $img in
  *.gz) gzip -dc "$img" > "$tmp/system.img"; img=$tmp/system.img ;;
esac
for module in com.android.runtime com.google.android.art; do
  out=$dest/apex/${module#com.google.android.}
  case $module in com.google.android.art) out=$dest/apex/com.android.art ;; *) out=$dest/apex/$module ;; esac
  debugfs -R "dump /system/apex/$module.apex $tmp/$module.apex" "$img" >/dev/null 2>&1 ||
    { echo "$0: no /system/apex/$module.apex in $img" >&2; exit 1; }
  mkdir -p "$tmp/$module"
  unzip -oq "$tmp/$module.apex" apex_payload.img -d "$tmp/$module"
  mkdir -p "$out"
  debugfs -R "rdump /lib64 $out" "$tmp/$module/apex_payload.img" >/dev/null 2>&1
  if [ "$module" = com.android.runtime ]; then
    mkdir -p "$out/bin"
    debugfs -R "dump /bin/linker64 $out/bin/linker64" "$tmp/$module/apex_payload.img" >/dev/null 2>&1
    [ -s "$out/bin/linker64" ] ||
      { echo "$0: no /bin/linker64 in $module" >&2; exit 1; }
  fi
  echo "$module -> $out/lib64"
done
mkdir -p "$dest/system/bin"
debugfs -R "dump /system/bin/app_process64 $dest/system/bin/app_process64" "$img" >/dev/null 2>&1
[ -s "$dest/system/bin/app_process64" ] ||
  { echo "$0: no /system/bin/app_process64 in $img" >&2; exit 1; }
mkdir -p "$dest/system/lib64"
for lib in libc++.so liblog.so libstdc++.so libz.so; do
  out=$tmp/$lib
  debugfs -R "dump /system/lib64/$lib $out" "$img" >/dev/null 2>&1
  [ -s "$out" ] ||
    { echo "$0: no /system/lib64/$lib in $img" >&2; exit 1; }
  cp "$out" "$dest/system/lib64/$lib"
done
