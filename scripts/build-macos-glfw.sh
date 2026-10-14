#!/bin/sh
# Build GLFW's Cocoa backend without requiring CMake/Homebrew.  The source is
# fetched by fetch-macos-deps.sh and the result stays in ignored .deps/.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/.deps/glfw"
obj="$root/.deps/glfw-build"
out="$root/.deps/lib/libglfw3.a"

[ -f "$src/include/GLFW/glfw3.h" ] || "$root/scripts/fetch-macos-deps.sh"
mkdir -p "$obj" "$(dirname "$out")"

sources='context.c init.c input.c monitor.c platform.c vulkan.c window.c
egl_context.c osmesa_context.c null_init.c null_monitor.c null_window.c null_joystick.c
cocoa_time.c posix_module.c posix_thread.c
cocoa_init.m cocoa_joystick.m cocoa_monitor.m cocoa_window.m nsgl_context.m'

objects=
for file in $sources; do
    name=${file%.*}.o
    target="$obj/$name"
    xcrun clang -O2 -fPIC -D_GLFW_COCOA -I"$src/include" -I"$src/src" \
        -c "$src/src/$file" -o "$target"
    objects="$objects $target"
done

# shellcheck disable=SC2086
libtool -static -o "$out" $objects
printf '%s\n' "built $out"
