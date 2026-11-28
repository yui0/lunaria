# Sourced by dist-cross-macos.sh. Install missing host tools and cache the
# target SDK, signer and static OpenSSL separately from native dependencies.
cache=${MAC_CROSS_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/lunaria/macos-arm64}
mkdir -p "$cache"
cache=$(realpath "$cache")
exec {setup_lock}>"$cache/setup.lock"
flock "$setup_lock"
tmp=$(mktemp -d "$cache/setup.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
host_bin="$cache/bin-$(uname -m)"
mkdir -p "$host_bin"
export PATH="$host_bin:$PATH"

download() {
    curl --fail --location --retry 3 --output "$2" "$1"
}

install_host_packages() {
    local deb="$1" rpm="$2"
    local as_root=()
    (( EUID == 0 )) || as_root=(sudo)
    if command -v apt-get >/dev/null; then
        "${as_root[@]}" apt-get update
        "${as_root[@]}" apt-get install -y $deb
    elif command -v dnf >/dev/null; then
        "${as_root[@]}" dnf install -y --setopt=excludepkgs= --setopt=exclude= $rpm
    else
        fail "install the host packages: $deb"
    fi
}

if ! command -v "${MAC_CC:-oa64-clang}" >/dev/null && [[ -z ${MAC_CC:-} ]]; then
    for tool in clang clang++ ld64.lld llvm-ar llvm-ranlib llvm-objcopy llvm-objdump; do
        command -v "$tool" >/dev/null && continue
        install_host_packages "clang lld llvm" "clang lld llvm"
        break
    done
    for driver in clang clang++; do
        wrapper="$host_bin/arm64-macos-$driver"
        cat >"$tmp/compiler" <<WRAPPER
#!/bin/sh
exec $driver --target=arm64-apple-darwin -isysroot "\$SDKROOT" -mmacosx-version-min=13.0 -fuse-ld=lld -Wno-unused-command-line-argument "\$@"
WRAPPER
        chmod +x "$tmp/compiler"
        mv "$tmp/compiler" "$wrapper"
    done
    MAC_CC="$host_bin/arm64-macos-clang"
    MAC_CXX=${MAC_CXX:-$host_bin/arm64-macos-clang++}
    # LLVM selects the cctools-compatible interface from argv[0].
    ln -sfn "$(command -v llvm-objdump)" "$host_bin/llvm-otool"
    ln -sfn "$(command -v llvm-objcopy)" "$host_bin/llvm-install-name-tool"
    MAC_OTOOL=${MAC_OTOOL:-llvm-otool}
    MAC_INSTALL_NAME_TOOL=${MAC_INSTALL_NAME_TOOL:-llvm-install-name-tool}
    export MAC_RANLIB=llvm-ranlib
fi

if [[ -z ${SDKROOT:-} ]]; then
    for sdk in "$HOME/osxcross/target/SDK"/MacOSX*.sdk /opt/osxcross/target/SDK/MacOSX*.sdk \
        /usr/local/osxcross/SDK/MacOSX*.sdk "$cache/MacOSX15.5.sdk"; do
        [[ -s "$sdk/usr/lib/libz.tbd" ]] && { SDKROOT=$sdk; break; }
    done
    if [[ -z ${SDKROOT:-} ]]; then
        archive=MacOSX15.5.sdk.tar.xz
        download "https://github.com/joseluisq/macosx-sdks/releases/download/15.5/$archive" "$tmp/$archive"
        echo "c15cf0f3f17d714d1aa5a642da8e118db53d79429eb015771ba816aa7c6c1cbd  $tmp/$archive" | sha256sum -c -
        tar --no-same-owner -xJf "$tmp/$archive" -C "$tmp"
        SDKROOT="$cache/MacOSX15.5.sdk"
        mv "$tmp/MacOSX15.5.sdk" "$SDKROOT"
    fi
fi
export SDKROOT OSXCROSS_SDKROOT="$SDKROOT"

if ! command -v rcodesign >/dev/null; then
    signer="apple-codesign-0.29.0-$(uname -m)-unknown-linux-musl"
    base=https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign%2F0.29.0
    download "$base/$signer.tar.gz" "$tmp/signer.tar.gz"
    download "$base/$signer.tar.gz.sha256" "$tmp/signer.sha256"
    echo "$(awk 'NR==1 {print $1}' "$tmp/signer.sha256")  $tmp/signer.tar.gz" | sha256sum -c -
    tar --no-same-owner -xzf "$tmp/signer.tar.gz" -C "$tmp"
    binary=$(find "$tmp" -type f -name rcodesign -print -quit)
    install -m 755 "$binary" "$tmp/rcodesign.new"
    mv "$tmp/rcodesign.new" "$host_bin/rcodesign"
fi

if [[ -z ${MAC_OPENSSL:-} ]]; then
    version=3.5.6
    MAC_OPENSSL="$cache/openssl-$version"
    if [[ ! -s "$MAC_OPENSSL/lib/libssl.a" ]]; then
        perl -MFindBin -MFile::Compare -MIPC::Cmd -MPod::Html -e 1 2>/dev/null ||
            install_host_packages perl "perl perl-FindBin perl-File-Compare perl-IPC-Cmd perl-Pod-Html"
        archive="openssl-$version.tar.gz"
        base="https://github.com/openssl/openssl/releases/download/openssl-$version"
        download "$base/$archive" "$tmp/$archive"
        echo "deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736  $tmp/$archive" | sha256sum -c -
        tar --no-same-owner -xzf "$tmp/$archive" -C "$tmp"
        (
            cd "$tmp/openssl-$version"
            export CC=${MAC_CC:-oa64-clang} AR=${MAC_AR:-llvm-ar}
            export RANLIB=${MAC_RANLIB:-llvm-ranlib} MACOSX_DEPLOYMENT_TARGET=13.0
            ./Configure darwin64-arm64-cc no-shared no-tests no-module --prefix="$MAC_OPENSSL" --libdir=lib
            make -j"${JOBS:-4}" build_libs
            make install_dev DESTDIR="$tmp/stage"
        )
        mv "$tmp/stage$MAC_OPENSSL" "$MAC_OPENSSL"
    fi
fi
# dynarmic generates its macOS exception server with Apple's `mig`, which does
# not exist on Linux. Build migcom from Apple's open-source bootstrap_cmds and
# install a `mig` wrapper that preprocesses with the target SDK.
if [[ ! -x "$host_bin/mig" || ! -x "$host_bin/migcom" ]]; then
    for tool in flex bison; do
        command -v "$tool" >/dev/null || { install_host_packages "flex bison" "flex bison"; break; }
    done
    command -v "${HOSTCC:-cc}" >/dev/null || install_host_packages "gcc" "gcc"
    archive=bootstrap_cmds-138.tar.gz
    download "https://github.com/apple-oss-distributions/bootstrap_cmds/archive/refs/tags/bootstrap_cmds-138.tar.gz" "$tmp/$archive"
    echo "7f86f67c13be04504679170e923c6079e706d57b295f10db752f2f8245f7df8c  $tmp/$archive" | sha256sum -c -
    tar --no-same-owner -xzf "$tmp/$archive" -C "$tmp"
    (
        cd "$tmp"/bootstrap_cmds-*/migcom.tproj
        # `bool` is a macro once <stdbool.h> is in scope; rename the parameter.
        sed -i 's/boolean_t bool\b/boolean_t b_/; s/if (bool)/if (b_)/' string.c strdefs.h
        flex -o lexxer.c lexxer.l
        bison -y -d -o parser.c parser.y
        cp parser.h y.tab.h
        shim="$OLDPWD/scripts/mig-shim"
        # Apple's Mach headers come last so they only fill in what glibc lacks.
        "${HOSTCC:-cc}" -w -O1 -fcommon -D_DEFAULT_SOURCE -D__LITTLE_ENDIAN__=1 \
            -D__private_extern__= '-DMIG_VERSION="bootstrap_cmds-138"' \
            -include "$shim/pre.h" -include stdlib.h -include string.h -include unistd.h \
            -I. -I"$shim" -idirafter "$SDKROOT/usr/include" -o "$tmp/migcom" \
            error.c global.c header.c mig.c routine.c server.c statement.c string.c \
            type.c user.c utils.c lexxer.c parser.c
    )
    install -m 755 "$tmp/migcom" "$host_bin/migcom"
    cat >"$tmp/mig" <<'MIG'
#!/bin/sh
# Minimal Linux `mig`: mig [-arch A] [-user F] [-header F] [-server F] [-sheader F] file.defs
set -eu
arch=arm64 flags=
while [ $# -gt 1 ]; do
    case $1 in
        -arch) arch=$2; shift 2 ;;
        -user|-header|-server|-sheader|-iheader|-dheader) flags="$flags $1 $2"; shift 2 ;;
        *) shift ;;
    esac
done
case $arch in arm64*) arch=arm64 ;; esac
here=$(dirname "$0")
clang -E -x c "--target=$arch-apple-darwin" -isysroot "$SDKROOT" -D__MACH30__ "$1" | "$here/migcom" $flags
MIG
    install -m 755 "$tmp/mig" "$host_bin/mig"
fi
rm -rf "$tmp"
trap - EXIT
exec {setup_lock}>&-
