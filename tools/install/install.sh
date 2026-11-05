#!/bin/sh
# Installs Calx on Linux:
#
#     curl -fsSL https://calxlang.com/install.sh | sh
#
# It downloads the newest release from github.com/ellipog/calx, checks its SHA-256, unpacks it
# into ~/.calx, and puts ~/.calx/bin on your PATH. No root is needed. Run it again to reinstall;
# `calx update` keeps an installation up to date.
#
# CALX_INSTALL installs somewhere else, CALX_CHANNEL=nightly picks the nightly releases,
# CALX_NO_MODIFY_PATH=1 leaves PATH alone, and CALX_RELEASES points at a mirror.

set -eu

stop() {
    printf 'calx install: %s\n' "$1" >&2
    exit 1
}

case "$(uname -s) $(uname -m)" in
    "Linux x86_64") ;;
    *) stop "Calx releases are for 64-bit x86 Linux and Windows; on $(uname -s) $(uname -m), build it from source: https://calxlang.com/guide/install/#building-from-source" ;;
esac

if command -v curl > /dev/null 2>&1; then
    get() { curl -fsSL -A calx-install "$1"; }
    get_to() { curl -fsSL -A calx-install -o "$2" "$1"; }
elif command -v wget > /dev/null 2>&1; then
    get() { wget -qO- -U calx-install "$1"; }
    get_to() { wget -qO "$2" -U calx-install "$1"; }
else
    stop "this needs curl or wget to download Calx"
fi
if command -v sha256sum > /dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum > /dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    stop "this needs sha256sum to check the download"
fi

releases=${CALX_RELEASES:-https://github.com/ellipog/calx/releases}
releases=${releases%/}
root=${CALX_INSTALL:-$HOME/.calx}

# The newest version: the latest stable release, or the nightly when asked for, or while there's
# no stable release yet.
version=""
if [ "${CALX_CHANNEL:-}" != nightly ]; then
    version=$(get "$releases/latest/download/version.txt" 2> /dev/null | head -n1 | tr -d '\r') || version=""
    if [ -z "$version" ]; then
        echo "There's no stable release yet, so this installs the newest nightly."
    fi
fi
if [ -z "$version" ]; then
    version=$(get "$releases/download/nightly/version.txt" | head -n1 | tr -d '\r') || stop "can't reach $releases"
fi
case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) stop "the release's version.txt doesn't hold a version" ;;
esac
case "$version" in
    *-nightly.*) tag=nightly ;;
    *) tag="v$version" ;;
esac
file="calx-$version-linux-x64.tar.gz"
url="$releases/download/$tag/$file"

temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
echo "Downloading Calx $version..."
get_to "$url" "$temp/$file" || stop "can't download $url"
want=$(get "$url.sha256" | cut -d' ' -f1) || stop "can't download $url.sha256"
got=$(sha256 "$temp/$file")
if [ "$got" != "$want" ]; then
    stop "the download's SHA-256 is $got, but the release says $want. Nothing was installed; try again in a minute."
fi
echo "Checked its SHA-256 ($(( $(wc -c < "$temp/$file") / 1048576 )) MB)."

mkdir -p "$temp/unpacked"
tar -xzf "$temp/$file" -C "$temp/unpacked"
new="$temp/unpacked/calx"
[ -x "$new/bin/calx" ] || stop "the download has no bin/calx"

# What's there from an earlier install moves aside first, and the release moves in.
mkdir -p "$root"
aside="$root/.update/replaced-$$"
for part in "$new"/* ; do
    name=$(basename "$part")
    if [ -e "$root/$name" ]; then
        mkdir -p "$aside"
        mv "$root/$name" "$aside/$name"
    fi
    mv "$part" "$root/$name"
done
rm -rf "$aside"

bin="$root/bin"
if [ "${CALX_NO_MODIFY_PATH:-}" != 1 ]; then
    case ":$PATH:" in
        *":$bin:"*) ;;
        *)
            line="export PATH=\"$bin:\$PATH\""
            added=""
            for rc in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
                # .profile always; the others only if the shell they're for is set up.
                if [ "$rc" = "$HOME/.profile" ] || [ -f "$rc" ]; then
                    if ! grep -qF "$line" "$rc" 2> /dev/null; then
                        printf '\n# Calx\n%s\n' "$line" >> "$rc"
                        added="$added $rc"
                    fi
                fi
            done
            if [ -n "$added" ]; then
                echo "Added $bin to your PATH in$added."
            fi
            ;;
    esac
fi

"$bin/calx" version
echo
echo "Calx is installed in $root."

# Calx builds programs with clang. Until releases carry their own (CLX-110), it has to be there.
if [ -z "${CALX_CC:-}" ] && ! command -v clang > /dev/null 2>&1; then
    echo
    echo "Calx builds programs with clang, which isn't installed yet. On Ubuntu or Debian:"
    echo "    sudo apt install clang lld"
fi
if ! ldconfig -p 2> /dev/null | grep -q 'libcurl'; then
    echo
    echo "\`calx update\` downloads with libcurl, which isn't installed. On Ubuntu or Debian:"
    echo "    sudo apt install libcurl4"
fi
echo
echo "Try it in a new terminal:  calx run hello.calx"
echo "Keep it up to date with:   calx update"
