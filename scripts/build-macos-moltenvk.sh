#!/bin/sh
# Rebuild the existing MoltenVK dependency with its fragment-output compiler fix.
# Uses the CMake, compiler and Python already required by the macOS build.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
deps="$root/.deps"
sources="$deps/moltenvk-sources"
cache="$deps/moltenvk-source-archives"
build="$deps/moltenvk-build"
patch_file="$root/scripts/patches/spirv-cross-fragment-output-mask.patch"
cmake_file="$root/scripts/macos-moltenvk/CMakeLists.txt"
version=1.4.2
arch=$(uname -m)
cmake="$deps/bin/cmake"
stamp="$deps/lib/.moltenvk-version"
out="$deps/lib/libMoltenVK.dylib"
fingerprint=$( { for file in "$0" "$patch_file" "$cmake_file"; do shasum -a 256 "$file" | awk '{print $1}'; done; printf '%s\n' "$arch"; } | shasum -a 256 | awk '{print $1}')
expected="$version-privateapi-fragment-output-$fingerprint"
if [ -f "$out" ] && [ -f "$deps/moltenvk/include/vulkan/vulkan.h" ] && \
   [ -f "$deps/moltenvk/include/vk_video/vulkan_video_codecs_common.h" ] && \
   [ "$(cat "$stamp" 2>/dev/null || true)" = "$expected" ]; then
    exit 0
fi
[ -x "$cmake" ] || { echo 'Run scripts/fetch-macos-deps.sh first (existing CMake is missing).' >&2; exit 1; }
mkdir -p "$sources" "$cache" "$deps/lib" "$deps/moltenvk/include"

fetch_source() {
    name=$1 repository=$2 revision=$3 digest=$4
    archive="$cache/$name.tar.gz"
    if [ ! -f "$archive" ] || [ "$(shasum -a 256 "$archive" | awk '{print $1}')" != "$digest" ]; then
        curl --fail --location --silent --show-error \
            "https://github.com/$repository/archive/$revision.tar.gz" --output "$archive.tmp"
        actual=$(shasum -a 256 "$archive.tmp" | awk '{print $1}')
        [ "$actual" = "$digest" ] || { rm -f "$archive.tmp"; echo "$name source checksum mismatch" >&2; exit 1; }
        mv "$archive.tmp" "$archive"
    fi
    if [ "$(cat "$sources/$name/.lunaria-source-sha256" 2>/dev/null || true)" != "$digest" ]; then
        unpack=$(mktemp -d "$sources/.unpack-$name.XXXXXX")
        tar -xzf "$archive" -C "$unpack" --strip-components=1
        printf '%s\n' "$digest" > "$unpack/.lunaria-source-sha256"
        rm -rf "$sources/$name"
        mv "$unpack" "$sources/$name"
    fi
}

# Revisions come from MoltenVK 1.4.2's ExternalRevisions files. These are the
# release's existing internal components, not additional runtime libraries.
fetch_source MoltenVK KhronosGroup/MoltenVK refs/tags/v1.4.2 \
    6864db532f1dbbdb621a8d0ec13f24edae318fd9269dd3dd0cdff791334bb1cb
fetch_source SPIRV-Cross KhronosGroup/SPIRV-Cross 6c09849fe88c48eaed08413aa022aaa136a3a057 \
    b81b9956289950570953738e666a031ca32ff64e4fc925eba89f227c42109518
fetch_source SPIRV-Tools KhronosGroup/SPIRV-Tools 0d6fd73ca73830ccab5fa1f00ed5ed40124e2c55 \
    20219c0962dffb0d7644bb3116a5923cfcf0526a569baa0b2a17343dd2fa6e5d
fetch_source SPIRV-Headers KhronosGroup/SPIRV-Headers 29981f65241605e08b0ede4cfeb999fe3b723c6a \
    232899f1ad4104fb5bc377b94596c7621575eee62ad9a9e8f929b63a7dd8a7ad
fetch_source Vulkan-Headers KhronosGroup/Vulkan-Headers e3b1eec08173d6b825cd3ac88c885a63b621504a \
    f492279345cbc10708b64fcd432b3ff6c8246a5837c4db2b649abba00cf82208
fetch_source cereal USCiLab/cereal a56bad8bbb770ee266e930c95d37fff2a5be7fea \
    12e10fb541270632f6683286ce8c7e8c321fe4addb53fa91ed498b95c1758f93

cross="$sources/SPIRV-Cross"
if [ ! -f "$cross/spirv_msl.cpp.lunaria-original" ]; then
    cp "$cross/spirv_msl.cpp" "$cross/spirv_msl.cpp.lunaria-original"
fi
cp "$cross/spirv_msl.cpp.lunaria-original" "$cross/spirv_msl.cpp"
patch -s -p1 -d "$cross" -i "$patch_file"
"$cmake" -S "$root/scripts/macos-moltenvk" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DLUNARIA_MVK_SOURCES="$sources" -DLUNARIA_MVK_FINGERPRINT="$fingerprint"
"$cmake" --build "$build" --target MoltenVK -j "${LUNARIA_BUILD_JOBS:-4}"

cp "$build/moltenvk/MoltenVK/libMoltenVK.dylib" "$out.tmp"
install_name_tool -id @rpath/libMoltenVK.dylib "$out.tmp"
codesign --force --sign - --timestamp=none "$out.tmp"
mv "$out.tmp" "$out"
cp -R "$sources/Vulkan-Headers/include/vulkan" "$deps/moltenvk/include/"
cp -R "$sources/Vulkan-Headers/include/vk_video" "$deps/moltenvk/include/"

licenses="$deps/moltenvk-licenses"
mkdir -p "$licenses"
for component in MoltenVK SPIRV-Cross SPIRV-Tools SPIRV-Headers Vulkan-Headers cereal; do
    mkdir -p "$licenses/$component"
    for item in LICENSE LICENSE.md LICENSES; do
        [ ! -e "$sources/$component/$item" ] || cp -R "$sources/$component/$item" "$licenses/$component/"
    done
done
printf 'MoltenVK %s; SPIRV-Cross fragment-output-mask repair.\nBuild fingerprint: %s\n' \
    "$version" "$fingerprint" > "$licenses/LUNARIA-CHANGES.txt"
printf '%s\n' "$expected" > "$stamp"
printf '%s\n' "built repaired MoltenVK $version ($arch)"
