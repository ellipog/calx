#!/bin/sh
# Installs Calx on Linux and macOS:
#
#     curl -fsSL https://calxlang.com/install.sh | sh
#
# It downloads the newest release from github.com/ellipog/calx, and the toolchain it builds
# programs with, clang and lld, unless that's installed already, checks their SHA-256s, unpacks
# them into ~/.calx, and puts ~/.calx/bin on your PATH. No root is needed. Run it again to
# reinstall; `calx update` keeps an installation up to date. On macOS, programs build against
# Apple's SDK, from the Command Line Tools, which `xcode-select --install` installs.
#
# CALX_INSTALL installs somewhere else, CALX_CHANNEL=nightly picks the nightly releases,
# CALX_NO_MODIFY_PATH=1 leaves PATH alone, and CALX_RELEASES points at a mirror.

set -eu

# How it looks (CLX-250), as calx does: in a terminal someone's watching, colour, a check mark
# before each step that's done, a progress bar for each download and a pulse while the toolchain
# unpacks. In a pipe, a file, CI or with NO_COLOR set, the plain lines it always printed.
# CLICOLOR_FORCE=1 or FORCE_COLOR turns the terminal's look on anyway, for a recording.
styled=""
if [ "${CLICOLOR_FORCE:-}" = 1 ] || { [ -n "${FORCE_COLOR:-}" ] && [ "${FORCE_COLOR:-}" != 0 ]; }; then
    styled=1
elif [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != dumb ] && [ -z "${CI:-}" ]; then
    styled=1
fi
esc=$(printf '\033')
# Calx's amber, the brand's (`--calx` on calxlang.com), as calx's, or the nearest the terminal's
# palette has; ground_sgr is the same behind text, and chip_sgr the ink of a chip's text.
case "${COLORTERM:-}" in
    truecolor | 24bit) accent_sgr='38;2;188;133;54' ground_sgr='48;2;188;133;54' chip_sgr='38;2;24;24;27;1' ;;
    *)
        case "${TERM:-}" in
            *256*) accent_sgr='38;5;136' ground_sgr='48;5;136' chip_sgr=30 ;;
            *) accent_sgr=33 ground_sgr=43 chip_sgr=30 ;;
        esac
        ;;
esac
unicode=""
# The marks, written as bytes since a terminal that isn't UTF-8 gets ASCII ones.
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *UTF-8* | *utf-8* | *UTF8* | *utf8*)
        tick=$(printf '\342\234\223') cross=$(printf '\342\234\227') dot=$(printf '\302\267')
        full=$(printf '\342\226\210') shade1=$(printf '\342\226\221') shade2=$(printf '\342\226\222') shade3=$(printf '\342\226\223')
        unicode=1
        ;;
    *) tick='+' cross='x' dot='-' full='#' shade1='-' shade2='=' shade3='=' ;;
esac

paint() {
    if [ -n "$styled" ]; then printf '%s[%sm%s%s[0m' "$esc" "$1" "$2" "$esc"; else printf '%s' "$2"; fi
}
accent() { paint "$accent_sgr" "$1"; }
dim() { paint 90 "$1"; }
done_line() { printf '%s %s\n' "$(paint 32 "$tick")" "$1"; }
# Bytes as megabytes with one decimal.
mb() {
    tenths=$(( ($1 * 10 + 524288) / 1048576 ))
    printf '%d.%d' $(( tenths / 10 )) $(( tenths % 10 ))
}
# Milliseconds since $1, a time from `now`.
now() { date +%s%N; }
since() { echo $(( ($(now) - $1) / 1000000 )); }

# The shade for $1 quarters of a cell in ink, in $s.
shade() {
    case $1 in 0 | 1) s=$shade1 ;; 2) s=$shade2 ;; 3) s=$shade3 ;; *) s=$full ;; esac
}

# Cells of shade, each amber ($1 is 1) or the grey of a track, with one escape code for each run:
# `cell LIT SHADE` adds one to $cells, and `cells_done` prints them.
cell() {
    if [ "$1" != "$lit" ]; then
        if [ "$1" = 1 ]; then cells="$cells$esc[0m$esc[${accent_sgr}m"; else cells="$cells$esc[0m$esc[90m"; fi
        lit=$1
    fi
    cells="$cells$2"
}
cells_done() { printf '%s%s[0m' "$cells" "$esc"; }

# A bar $3 cells wide, $1 of $2 filled, as calx's: full cells, then an edge that fades through
# the shades over four cells, on a grey track, breathing with frame $4. In hundredths of a cell,
# since sh has only whole numbers; the breathing is 0.6 sin(0.7 frame), from a table.
bar() {
    got=$1
    [ "$got" -le "$2" ] || got=$2
    head=$(( -150 + got * ($3 + 3) * 100 / $2 ))
    if [ "$got" -gt 0 ] && [ "$got" -lt "$2" ]; then
        case $(( $4 % 9 )) in 1) head=$(( head + 39 )) ;; 2) head=$(( head + 59 )) ;; 3) head=$(( head + 52 )) ;; 4) head=$(( head + 20 )) ;;
            5) head=$(( head - 21 )) ;; 6) head=$(( head - 52 )) ;; 7) head=$(( head - 59 )) ;; 8) head=$(( head - 38 )) ;; esac
    fi
    cells="" lit="" i=0
    while [ $i -lt "$3" ]; do
        v=$(( (head - i * 100 - 50) / 4 + 50 ))
        [ $v -ge 0 ] || v=0
        [ $v -le 100 ] || v=100
        q=$(( (v * 4 + 50) / 100 ))
        if [ $q -gt 0 ]; then shade $q; cell 1 "$s"; else cell 0 "$shade1"; fi
        i=$(( i + 1 ))
    done
    cells_done
}

# A comet of dither that sweeps to and fro across a grey track 8 cells wide, at frame $1, as
# calx's.
scanner() {
    at=$(( $1 % 14 ))
    cells="" lit="" i=0
    while [ $i -lt 8 ]; do
        if [ $at -lt 7 ]; then behind=$(( at - i )); else behind=$(( i - (14 - at) )); fi
        if [ $behind -ge 0 ] && [ $behind -lt 4 ]; then shade $(( 4 - behind )); cell 1 "$s"; else cell 0 "$shade1"; fi
        i=$(( i + 1 ))
    done
    cells_done
}

# A result: $1 on a chip of amber that dithers in and out, as calx's.
band() {
    printf '%s[%sm%s%s%s%s[0m%s[%s;%sm %s %s[0m%s[%sm%s%s%s%s[0m' "$esc" "$accent_sgr" "$shade1" "$shade2" "$shade3" "$esc" \
        "$esc" "$ground_sgr" "$chip_sgr" "$1" "$esc" "$esc" "$accent_sgr" "$shade3" "$shade2" "$shade1" "$esc"
}

# The Calx mark, as calx draws it, with the seven lines given beside it. It dissolves in: each cell
# fades up to its shade in its turn, in the order a 4 by 4 Bayer matrix gives, over four tenths
# of a second. Without UTF-8 there's no mark, just the lines.
show_mark() {
    if [ -z "$unicode" ]; then
        for line in "$@"; do [ -z "$line" ] || printf '%s\n' "$line"; done
        return
    fi
    f=1
    while [ $f -le 10 ]; do
        t=$(( f * 10 ))
        [ $f -eq 1 ] || printf '%s[7A' "$esc"
        r=0
        for row in "0 0 0 2 3 4 4 4 4 3 2 1 0 0 0 0 0 1 3 4 4 2" "0 2 4 4 4 4 3 3 3 4 4 4 3 1 0 1 3 4 4 4 3 1" \
            "2 4 4 4 2 0 0 0 0 1 3 4 3 2 3 4 4 4 3 1 0 0" "3 4 4 3 0 0 0 0 0 0 0 1 3 4 4 4 3 2 0 0 0 0" \
            "2 4 4 4 2 0 0 0 0 1 3 4 4 4 3 2 2 4 3 1 0 0" "0 2 4 4 4 4 3 3 3 4 4 4 3 1 0 1 3 4 4 4 3 1" \
            "0 0 0 2 3 4 4 4 4 3 2 1 0 0 0 0 0 1 3 4 4 2"; do
            case $(( r % 4 )) in 0) b0=0 b1=8 b2=2 b3=10 ;; 1) b0=12 b1=4 b2=14 b3=6 ;; 2) b0=3 b1=11 b2=1 b3=9 ;; *) b0=15 b1=7 b2=13 b3=5 ;; esac
            line="" c=0
            for level in $row; do
                case $(( c % 4 )) in 0) threshold=$b0 ;; 1) threshold=$b1 ;; 2) threshold=$b2 ;; *) threshold=$b3 ;; esac
                p=$(( (t - 60 * threshold / 16) * 100 / 40 ))
                [ $p -ge 0 ] || p=0
                [ $p -le 100 ] || p=100
                shown=$(( (level * p + 50) / 100 ))
                if [ $shown -eq 0 ]; then line="$line "; else shade $shown; line="$line$s"; fi
                c=$(( c + 1 ))
            done
            r=$(( r + 1 ))
            eval "beside=\${$r:-}"
            [ -z "$beside" ] || beside="   $beside"
            printf '\r%s[2K%s[%sm%s%s[0m%s\n' "$esc" "$esc" "$accent_sgr" "$line" "$esc" "$beside"
        done
        [ $f -eq 10 ] || sleep 0.04 2> /dev/null || true
        f=$(( f + 1 ))
    done
}

# Downloads $1 to the file $2, and in a terminal, once a quarter of a second has passed, draws $3,
# a bar of how much has come and the speed, over the line, so a quick download draws nothing.
get_shown() {
    if [ -z "$styled" ] || ! command -v curl > /dev/null 2>&1; then
        get_to "$1" "$2"
        return
    fi
    total=$(curl -fsSIL -A calx-install "$1" 2> /dev/null | tr -d '\r' | awk 'tolower($1) == "content-length:" { n = $2 } END { print n + 0 }') || total=0
    curl -fsSL -A calx-install -o "$2" "$1" &
    pid=$!
    start=$(now) drawn="" frame=0
    while kill -0 "$pid" 2> /dev/null; do
        ms=$(since "$start")
        if [ "$ms" -ge 250 ]; then
            got=$(wc -c < "$2" 2> /dev/null || echo 0)
            speed=$(( (got * 10 / 1048576) * 1000 / ms ))
            rate=$(dim "$(( speed / 10 )).$(( speed % 10 )) MB/s")
            if [ "$total" -gt 0 ]; then
                printf '\r%s[2K  %s  %s  %s of %s MB  %s' "$esc" "$3" "$(bar "$got" "$total" 24 "$frame")" "$(mb "$got")" "$(mb "$total")" "$rate"
            else
                printf '\r%s[2K  %s  %s  %s MB  %s' "$esc" "$3" "$(scanner "$frame")" "$(mb "$got")" "$rate"
            fi
            drawn=1 frame=$(( frame + 1 ))
        fi
        sleep 0.08 2> /dev/null || sleep 1
    done
    status=0
    wait "$pid" || status=$?
    if [ -n "$drawn" ]; then printf '\r%s[2K' "$esc"; fi
    return $status
}

# Runs the command after $1, and in a terminal draws $1 with a scanner before it and the time so far
# after it while it runs, once a quarter of a second has passed.
pulse_while() {
    text=$1
    shift
    if [ -z "$styled" ]; then
        "$@"
        return
    fi
    "$@" &
    pid=$!
    start=$(now) drawn="" frame=0
    while kill -0 "$pid" 2> /dev/null; do
        ms=$(since "$start")
        if [ "$ms" -ge 250 ]; then
            time=""
            [ "$ms" -lt 1000 ] || time="  $(dim "$(( ms / 1000 )).$(( ms % 1000 / 100 )) s")"
            printf '\r%s[2K%s  %s%s' "$esc" "$(scanner "$frame")" "$text" "$time"
            drawn=1 frame=$(( frame + 1 ))
        fi
        sleep 0.08 2> /dev/null || sleep 1
    done
    status=0
    wait "$pid" || status=$?
    if [ -n "$drawn" ]; then printf '\r%s[2K' "$esc"; fi
    return $status
}

stop() {
    if [ -n "$styled" ]; then
        printf '%s calx install: %s\n' "$(paint 31 "$cross")" "$1" >&2
    else
        printf 'calx install: %s\n' "$1" >&2
    fi
    exit 1
}

# The release for the system and processor: x64, or Arm64, which a Raspberry Pi 5 or an Arm
# server runs, and which its release counts on ARMv8.2 from (D612), or a Mac with Apple Silicon,
# whose shell may say x86_64 when it runs under Rosetta.
os=linux
case "$(uname -s) $(uname -m)" in
    "Linux x86_64") cpu=x64 ;;
    "Linux aarch64" | "Linux arm64") cpu=arm64 ;;
    "Darwin arm64") os=macos cpu=arm64 ;;
    "Darwin x86_64")
        if [ "$(sysctl -n sysctl.proc_translated 2> /dev/null)" = 1 ]; then
            os=macos cpu=arm64
        else
            stop "Calx's macOS release is for Apple Silicon; on an Intel Mac, build it from source: https://calxlang.com/guide/install/#building-from-source"
        fi
        ;;
    *) stop "Calx releases are for Linux on x64 and Arm64, macOS on Apple Silicon, and Windows; on $(uname -s) $(uname -m), build it from source: https://calxlang.com/guide/install/#building-from-source" ;;
esac
# A Mac builds programs against Apple's SDK, which the Command Line Tools have. `xcode-select -p`
# says where they are without offering to install them.
if [ "$os" = macos ] && ! xcode-select -p > /dev/null 2>&1; then
    stop "Calx builds programs against Apple's SDK, from the Command Line Tools: run \`xcode-select --install\`, then this again"
fi

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
if [ -n "$styled" ]; then
    echo
    show_mark "" "" "$(paint 1 'Installing Calx')" "$(dim 'the compiler, its standard library,')" "$(dim 'and the toolchain it builds with')" "" ""
    echo
fi

# The newest version: the latest stable release, or the nightly when asked for, or while there's
# no stable release yet.
version=""
if [ "${CALX_CHANNEL:-}" != nightly ]; then
    version=$(get "$releases/latest/download/version.txt" 2> /dev/null | head -n1 | tr -d '\r') || version=""
    if [ -z "$version" ]; then
        if [ -n "$styled" ]; then
            printf '%s %s\n' "$(dim "$dot")" "$(dim "There's no stable release yet, so this installs the newest nightly.")"
        else
            echo "There's no stable release yet, so this installs the newest nightly."
        fi
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
file="calx-$version-$os-$cpu.tar.gz"
url="$releases/download/$tag/$file"

temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
if [ -z "$styled" ]; then echo "Downloading Calx $version..."; fi
get_shown "$url" "$temp/$file" "$file" || stop "can't download $url"
want=$(get "$url.sha256" | cut -d' ' -f1) || stop "can't download $url.sha256"
got=$(sha256 "$temp/$file")
if [ "$got" != "$want" ]; then
    stop "the download's SHA-256 is $got, but the release says $want. Nothing was installed; try again in a minute."
fi
if [ -n "$styled" ]; then
    done_line "Downloaded Calx $(accent "$version") $(dim "$(mb "$(wc -c < "$temp/$file")") MB, SHA-256 checked")"
else
    echo "Checked its SHA-256 ($(( $(wc -c < "$temp/$file") / 1048576 )) MB)."
fi

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
if [ -n "$styled" ]; then done_line "Unpacked it into $(accent "$root")"; fi

# The toolchain programs are built with, clang and lld, is a download of its own, since it changes
# only with LLVM (CLX-110, CLX-251): it's fetched when the one here isn't the one the release names
# in toolchain.txt, its file, SHA-256 and VERSION. Unpacked beside the installation, since /tmp
# may be too small a RAM disk for its 400 MB.
if [ -f "$root/toolchain.txt" ]; then
    tc_file=$(sed -n 1p "$root/toolchain.txt")
    tc_sum=$(sed -n 2p "$root/toolchain.txt")
    tc_name=$(sed -n 3p "$root/toolchain.txt")
    if [ "$(cat "$root/toolchain/VERSION" 2> /dev/null)" != "$tc_name" ]; then
        if [ -z "$styled" ]; then echo "Downloading the toolchain programs are built with, $tc_name..."; fi
        work="$root/.update/toolchain-install"
        rm -rf "$work"
        mkdir -p "$work"
        get_shown "$releases/download/$tag/$tc_file" "$work/$tc_file" toolchain || stop "can't download $releases/download/$tag/$tc_file"
        got=$(sha256 "$work/$tc_file")
        if [ "$got" != "$tc_sum" ]; then
            rm -rf "$work"
            stop "the toolchain's SHA-256 is $got, but the release says $tc_sum. Try again in a minute; \`calx update --toolchain\` does too."
        fi
        if [ -n "$styled" ]; then
            done_line "Downloaded the toolchain, $tc_name $(dim "$(mb "$(wc -c < "$work/$tc_file")") MB, SHA-256 checked")"
        else
            echo "Checked its SHA-256 ($(( $(wc -c < "$work/$tc_file") / 1048576 )) MB). Unpacking it..."
        fi
        pulse_while "Unpacking the toolchain" tar -xJf "$work/$tc_file" -C "$work" ||
            stop "can't unpack the toolchain, which needs xz: on Ubuntu or Debian, sudo apt install xz-utils"
        rm -rf "$root/toolchain"
        mv "$work/toolchain" "$root/toolchain"
        rm -rf "$work"
        if [ -n "$styled" ]; then done_line "Unpacked the toolchain"; fi
    fi
fi

bin="$root/bin"
if [ "${CALX_NO_MODIFY_PATH:-}" != 1 ]; then
    case ":$PATH:" in
        *":$bin:"*) ;;
        *)
            line="export PATH=\"$bin:\$PATH\""
            added=""
            # On Linux .profile always; the others only if the shell they're for is set up. On
            # macOS .zprofile always, since zsh is its shell and reads no .profile, and a Terminal
            # window starts a login shell; .bash_profile if bash is set up.
            if [ "$os" = macos ]; then
                always="$HOME/.zprofile"
                set -- "$HOME/.zprofile" "$HOME/.bash_profile"
            else
                always="$HOME/.profile"
                set -- "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"
            fi
            for rc in "$@"; do
                if [ "$rc" = "$always" ] || [ -f "$rc" ]; then
                    if ! grep -qF "$line" "$rc" 2> /dev/null; then
                        printf '\n# Calx\n%s\n' "$line" >> "$rc"
                        added="$added $rc"
                    fi
                fi
            done
            if [ -n "$added" ] && [ -n "$styled" ]; then
                done_line "Added $(accent "$bin") to your PATH in$added"
            elif [ -n "$added" ]; then
                echo "Added $bin to your PATH in$added."
            fi
            ;;
    esac
fi

if [ -n "$styled" ]; then
    # A first program, built and run, which checks it all works and compiles the runtime, so the
    # first program someone writes builds as quickly as the rest. calx draws what it's doing on
    # standard error meanwhile; the program's output isn't wanted.
    if [ -x "$root/toolchain/bin/clang" ] || [ -n "${CALX_CC:-}" ] || command -v clang > /dev/null 2>&1; then
        first="$root/.update/first"
        mkdir -p "$first"
        printf 'fn main() {\n    println("hello")\n}\n' > "$first/hello.calx"
        start=$(now)
        if "$bin/calx" run "$first/hello.calx" > /dev/null; then
            ms=$(since "$start")
            done_line "Built and ran a first program $(dim "$(( ms / 1000 )).$(( ms % 1000 / 100 )) s")"
        else
            printf '%s A first program didn'\''t build; %s says why.\n' "$(paint 33 '!')" "$(accent 'calx doctor')"
        fi
        rm -rf "$first"
    fi
    echo
    printf '%s\n%s\n' "$(band "$tick Calx $version is installed")" "$(dim "in $root")"
else
    "$bin/calx" version
    echo
    echo "Calx is installed in $root."
fi

# What Linux needs beside: macOS has libcurl, and the SDK was checked before the download.
if [ "$os" = linux ]; then
    # Calx builds programs with clang: the toolchain's, or one installed for a release without one.
    if [ ! -x "$root/toolchain/bin/clang" ] && [ -z "${CALX_CC:-}" ] && ! command -v clang > /dev/null 2>&1; then
        echo
        echo "Calx builds programs with clang, which isn't installed yet. On Ubuntu or Debian:"
        echo "    sudo apt install clang lld"
    fi
    # Programs use the system's C library, with its headers and start files.
    if [ ! -f /usr/include/stdio.h ]; then
        echo
        echo "Calx builds programs with the C library's headers, which aren't installed yet."
        echo "On Ubuntu or Debian:  sudo apt install libc6-dev"
        echo "On Fedora:            sudo dnf install glibc-devel"
    fi
    if ! ldconfig -p 2> /dev/null | grep -q 'libcurl'; then
        echo
        echo "\`calx update\` downloads with libcurl, which isn't installed. On Ubuntu or Debian:"
        echo "    sudo apt install libcurl4"
    fi
fi
echo
if [ -n "$styled" ]; then
    echo "Try it in a new terminal   $(accent 'calx run hello.calx')"
    echo "Keep it up to date         $(accent 'calx update')"
    echo "See what it can do         $(accent 'calx help')"
else
    echo "Try it in a new terminal:  $(accent 'calx run hello.calx')"
    echo "Keep it up to date with:   $(accent 'calx update')"
fi
