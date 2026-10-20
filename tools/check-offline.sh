#!/usr/bin/env bash
# check-offline.sh - hardware-free build, ABI, export and test verification.
#
# Runs only offline work. It never opens a serial port, loads a provider,
# contacts an adapter or a vehicle, installs a package, or uses the network.
# Every artifact goes to one isolated output directory that this script owns.
#
# Steps:
#   1. Native build. Uses CMake when it is available (and applies --cc to its
#      first configure); otherwise compiles the same sources directly.
#   2. Native tests with no arguments, from test/offline-tests.txt.
#   3. Fixed-width J2534 ABI assertions and the native export set.
#   4. Optional MinGW DLL builds and undecorated-export checks.
#   5. Optional ASAN/UBSAN builds and runs of the same manifest tests.
#
# The test manifest is shared with CMakeLists.txt, so the CMake, direct and
# sanitizer paths cannot drift. A host-specific test is reported as SKIP with
# the missing capability. A compile or test that starts and fails is FAIL and
# exits non-zero; it is never converted into a skip. The undocumented live-test
# argument is never passed.
#
# Usage:
#   tools/check-offline.sh [options]
#
# Options:
#   --out-dir DIR   Isolated output root (default: <repo>/build/check-offline)
#   --cc CC         Native C compiler (default: $CC, then cc)
#   --cmake PATH    CMake command (default: $CMAKE, then cmake)
#   --no-cmake      Use the direct compiler path; report the CMake step skipped
#   --no-mingw      Skip the optional Windows cross-build
#   --no-sanitize   Skip the optional sanitizer build
#   -h, --help      Show this text
#
# Exit status: 0 when every executed check passed and at least one check ran.
# 1 when any check failed. 2 for a usage or setup error.

set -uo pipefail

if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
    printf 'error: this checker needs Bash 4.3 or later; use CMake directly on this host\n' >&2
    exit 2
fi

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out_dir="$repo/build/check-offline"
cc="${CC:-cc}"
cmake_bin="${CMAKE:-cmake}"
nm_bin="${NM:-nm}"
want_cmake=auto
want_mingw=1
want_sanitize=1

usage() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next }
         NR > 1 { exit }' "${BASH_SOURCE[0]}"
}

usage_error() {
    printf 'error: %s\n\n' "$*" >&2
    usage >&2
    exit 2
}

need_value() {
    local option=$1
    shift
    if (($# < 2)) || [[ ${2-} == --* ]]; then
        usage_error "$option needs a value"
    fi
}

while (($#)); do
    case "$1" in
        --out-dir) need_value "$1" "$@"; out_dir=$2; shift 2 ;;
        --cc) need_value "$1" "$@"; cc=$2; shift 2 ;;
        --cmake) need_value "$1" "$@"; cmake_bin=$2; shift 2 ;;
        --no-cmake) want_cmake=0; shift ;;
        --no-mingw) want_mingw=0; shift ;;
        --no-sanitize) want_sanitize=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage_error "unknown option: $1" ;;
    esac
done

abs_path() {
    if command -v realpath >/dev/null 2>&1; then
        realpath -m -- "$1"
    else
        (cd "$(dirname "$1")" && printf '%s/%s\n' "$PWD" "$(basename "$1")")
    fi
}

if ! repo_abs=$(abs_path "$repo") || [[ -z $repo_abs ]]; then
    usage_error "cannot resolve the repository path; use CMake directly on this host"
fi
if ! out_abs=$(abs_path "$out_dir") || [[ -z $out_abs ]]; then
    usage_error "cannot resolve the output path; no output was cleaned"
fi

if [[ $out_abs == / ]]; then
    usage_error "refusing to use the filesystem root as the output directory"
fi
if [[ $out_abs == "$repo_abs" ]]; then
    usage_error "refusing to use the repository itself as the output directory"
fi
if [[ $repo_abs == "$out_abs"/* ]]; then
    usage_error "refusing an output directory that contains the repository: $out_abs"
fi
if [[ -e $out_abs && ! -d $out_abs ]]; then
    usage_error "output path exists and is not a directory: $out_abs"
fi
if [[ -d $out_abs && ! -f $out_abs/.mvci-check-offline ]] \
        && [[ -n $(ls -A "$out_abs" 2>/dev/null) ]]; then
    usage_error "refusing to clean a non-empty directory this script did not create: $out_abs"
fi

if ! command -v "$cc" >/dev/null 2>&1; then
    printf 'FAIL  native C compiler not found: %s\n' "$cc" >&2
    exit 2
fi

# The manifest is the single test list for CMake, the direct path and the
# sanitizer path. Refuse to run with an unreadable or malformed one rather
# than silently testing less.
manifest="$repo/test/offline-tests.txt"
if [[ ! -f $manifest ]]; then
    printf 'FAIL  offline test manifest not found: %s\n' "$manifest" >&2
    exit 2
fi

case "$(uname -s)" in
    Darwin) host_class=mac ;;
    MINGW*|MSYS*|CYGWIN*) host_class=win ;;
    *) host_class=gnu ;;
esac

host_supported() {
    case "$1" in
        any) return 0 ;;
        posix) [[ $host_class != win ]] ;;
        gnu) [[ $host_class == gnu ]] ;;
        *) return 1 ;;
    esac
}

mkdir -p "$out_abs" || exit 2
: >"$out_abs/.mvci-check-offline" || exit 2
rm -rf "$out_abs/logs" "$out_abs/native" "$out_abs/cmake-native" \
       "$out_abs/abi" "$out_abs/sanitize" "$out_abs/windows-32" \
       "$out_abs/windows-64"
mkdir -p "$out_abs/logs" || exit 2

pass_n=0
fail_n=0
skip_n=0
pass() { printf 'PASS  %s\n' "$*"; pass_n=$((pass_n + 1)); }
skip() { printf 'SKIP  %s\n' "$*"; skip_n=$((skip_n + 1)); }
fail() { printf 'FAIL  %s\n' "$*"; fail_n=$((fail_n + 1)); }
note() { printf '      %s\n' "$*"; }
section() { printf '\n== %s\n' "$*"; }
show_cmd() { printf '+'; printf ' %q' "$@"; printf '\n'; }

# run_logged <log> <command...>: run quietly, show the command, and print the
# tail of the log on failure. Returns the command's status.
run_logged() {
    local log=$1
    shift
    show_cmd "$@"
    if "$@" >"$log" 2>&1; then
        return 0
    else
        local status=$?
        printf '      log: %s\n' "$log"
        tail -n 20 "$log" | sed 's/^/      | /'
        return "$status"
    fi
}

# first_reason <log>: one line that explains a toolchain failure.
first_reason() {
    local line
    line=$(grep -m1 -E 'cannot find|cannot open|No such file|undefined reference|not found|ld: ' "$1" 2>/dev/null || true)
    printf '%s' "${line:-see log}"
}

# A missing sanitizer runtime is diagnosed from the linker message. Any other
# probe failure is a real build failure and must not be reported as a skip.
sanitizer_runtime_missing() {
    grep -qE 'cannot find -l(asan|ubsan|sanitizer)|libasan|libubsan|libclang_rt\.(asan|ubsan)' "$1"
}

cflags=(-std=gnu11 -O2 -Wall -Wextra -Wno-deprecated-declarations
        -I"$repo/include" -I"$repo/src")

link_flags=(-pthread)
if [[ $host_class != mac ]]; then
    link_flags=(-lcrypto "${link_flags[@]}")
fi

# Read and validate the manifest once. Malformed lines are setup errors.
manifest_lines=()
while IFS= read -r line || [[ -n $line ]]; do
    line=${line%$'\r'}
    [[ -z ${line//[[:space:]]/} ]] && continue
    [[ $line == \#* ]] && continue
    IFS='|' read -r m_name m_ctest m_sources m_defines m_includes m_wraps m_hosts m_note <<<"$line"
    if [[ -z ${m_name:-} || -z ${m_ctest:-} || -z ${m_sources:-} \
            || -z ${m_hosts:-} || -z ${m_note:-} ]]; then
        printf 'FAIL  malformed manifest line: %s\n' "$line" >&2
        exit 2
    fi
    case "$m_hosts" in
        any|posix|gnu) ;;
        *) printf 'FAIL  unknown host class %s in manifest line: %s\n' "$m_hosts" "$line" >&2; exit 2 ;;
    esac
    manifest_lines+=("$line")
done <"$manifest"

# split_field <field> <array-name>: '-' means empty; otherwise split on spaces.
split_field() {
    local value=$1
    local -n _out=$2
    _out=()
    [[ $value == "-" || -z $value ]] && return 0
    read -r -a _out <<<"$value"
}

printf 'libMVCI offline check\n'
printf '  repo:      %s\n' "$repo"
printf '  output:    %s\n' "$out_abs"
printf '  compiler:  %s (%s)\n' "$(command -v "$cc")" "$("$cc" --version 2>/dev/null | head -1)"
if command -v "$cmake_bin" >/dev/null 2>&1; then
    printf '  cmake:     %s\n' "$("$cmake_bin" --version 2>/dev/null | head -1)"
else
    printf '  cmake:     not found (%s)\n' "$cmake_bin"
fi
printf '  host:      %s\n' "$host_class"

ctest_bin=""
if command -v "$cmake_bin" >/dev/null 2>&1; then
    ctest_bin="$(dirname "$(command -v "$cmake_bin")")/ctest"
    if [[ ! -x $ctest_bin ]]; then
        ctest_bin=$(command -v ctest || true)
    fi
fi

# Report host-limited tests once for every path.
section "Platform test scope"
for line in "${manifest_lines[@]}"; do
    IFS='|' read -r m_name m_ctest m_sources m_defines m_includes m_wraps m_hosts m_note <<<"$line"
    if host_supported "$m_hosts"; then
        printf '  RUN   %s\n' "$m_name"
    else
        skip "$m_name: needs a $m_hosts host ($m_note)"
    fi
done

# --------------------------------------------------------------------------
section "Native build and hardware-free tests"
# --------------------------------------------------------------------------
native_dir="$out_abs/native"
mkdir -p "$native_dir"
native_lib=""
native_step_ok=0
ctest_ran=0

if [[ $want_cmake != 0 ]] && command -v "$cmake_bin" >/dev/null 2>&1; then
    cmake_dir="$out_abs/cmake-native"
    if run_logged "$out_abs/logs/cmake-configure.log" \
            "$cmake_bin" -S "$repo" -B "$cmake_dir" \
            -DCMAKE_BUILD_TYPE=Release -DMVCI_BUILD_TESTS=ON -DMVCI_INSTALL=OFF \
            -DCMAKE_C_COMPILER="$cc" \
        && run_logged "$out_abs/logs/cmake-build.log" \
            "$cmake_bin" --build "$cmake_dir" --parallel; then
        native_lib=$(find "$cmake_dir" -maxdepth 2 -type f \
            \( -name 'libMVCI.so*' -o -name 'libMVCI*.dylib' \) | sort | head -1)
        if [[ -z $native_lib ]]; then
            fail "CMake native build produced no libMVCI shared library"
        else
            native_step_ok=1
            if [[ -z $ctest_bin ]]; then
                skip "CTest not found next to $cmake_bin; the CMake build ran but no tests ran"
            else
                ctest_ran=1
                if run_logged "$out_abs/logs/ctest.log" \
                        "$ctest_bin" --test-dir "$cmake_dir" --output-on-failure; then
                    if grep -q 'No tests were found' "$out_abs/logs/ctest.log"; then
                        fail "CTest discovered no tests to run"
                    else
                        pass "CMake build and CTest (no-argument manifest tests)"
                    fi
                else
                    fail "CTest (see $out_abs/logs/ctest.log)"
                fi
            fi
        fi
    else
        fail "CMake configure/build (see $out_abs/logs)"
    fi
else
    if [[ $want_cmake == 0 ]]; then
        skip "CMake step disabled by --no-cmake; used the direct compiler path"
    else
        skip "CMake not found ('$cmake_bin' is not on PATH); used the direct compiler path"
    fi
fi

if [[ $ctest_ran == 0 ]]; then
    native_lib="$native_dir/libMVCI.so"
    [[ $host_class == mac ]] && native_lib="$native_dir/libMVCI.dylib"

    if run_logged "$out_abs/logs/native-lib.log" \
            "$cc" "${cflags[@]}" -fPIC -fvisibility=hidden -DMVCI_BUILDING_DLL \
            -shared "$repo/src/passthru.c" "$repo/src/serial.c" \
            "$repo/src/io.c" "$repo/src/des.c" "${link_flags[@]}" \
            -o "$native_lib"; then
        pass "native shared library build ($cc)"
        native_step_ok=1
    else
        fail "native shared library build (see $out_abs/logs/native-lib.log)"
    fi

    # build_direct_test <name> <sources...>
    build_direct_test() {
        local name=$1
        shift
        local def_args=() inc_args=() wrap_args=() word
        for word in "${def_words[@]}"; do def_args+=("-D$word"); done
        for word in "${inc_words[@]}"; do inc_args+=("-I$repo/$word"); done
        for word in "${wrap_words[@]}"; do wrap_args+=("-Wl,--wrap=$word"); done
        run_logged "$out_abs/logs/$name-build.log" \
            "$cc" "${cflags[@]}" "${def_args[@]}" "${inc_args[@]}" \
            "${wrap_args[@]}" "$@" "${link_flags[@]}" -o "$native_dir/$name"
    }

    for line in "${manifest_lines[@]}"; do
        IFS='|' read -r m_name m_ctest m_sources m_defines m_includes m_wraps m_hosts m_note <<<"$line"
        host_supported "$m_hosts" || continue
        split_field "$m_sources" source_words
        split_field "$m_defines" def_words
        split_field "$m_includes" inc_words
        split_field "$m_wraps" wrap_words
        source_paths=()
        for word in "${source_words[@]}"; do source_paths+=("$repo/$word"); done
        if build_direct_test "$m_name" "${source_paths[@]}"; then
            if run_logged "$out_abs/logs/$m_name.log" "$native_dir/$m_name"; then
                pass "$m_name (no arguments)"
            else
                fail "$m_name (no arguments)"
            fi
        else
            fail "$m_name build"
        fi
    done
fi

# --------------------------------------------------------------------------
section "Fixed-width J2534 ABI"
# --------------------------------------------------------------------------
abi_dir="$out_abs/abi"
mkdir -p "$abi_dir"
cat >"$abi_dir/abi_probe.c" <<'EOF'
/* Generated by tools/check-offline.sh. The J2534 ABI is fixed-width: these
 * assertions fail to compile when a header changes a scalar, field offset,
 * or message size. This is a compile-time check only. */
#include <mvci/j2534.h>
#include <stddef.h>

_Static_assert(sizeof(J2534_ULONG) == 4, "J2534_ULONG must be 32 bits");
_Static_assert(sizeof(J2534_LONG) == 4, "J2534_LONG must be 32 bits");
_Static_assert(sizeof(PASSTHRU_MSG) == 4152, "PASSTHRU_MSG size changed");
_Static_assert(offsetof(PASSTHRU_MSG, ProtocolID) == 0, "ProtocolID offset");
_Static_assert(offsetof(PASSTHRU_MSG, RxStatus) == 4, "RxStatus offset");
_Static_assert(offsetof(PASSTHRU_MSG, TxFlags) == 8, "TxFlags offset");
_Static_assert(offsetof(PASSTHRU_MSG, Timestamp) == 12, "Timestamp offset");
_Static_assert(offsetof(PASSTHRU_MSG, DataSize) == 16, "DataSize offset");
_Static_assert(offsetof(PASSTHRU_MSG, ExtraDataIndex) == 20, "ExtraDataIndex offset");
_Static_assert(offsetof(PASSTHRU_MSG, Data) == 24, "Data offset");
_Static_assert(sizeof(SCONFIG) == 8, "SCONFIG size changed");
_Static_assert(offsetof(SCONFIG, Value) == 4, "SCONFIG.Value offset");
_Static_assert(offsetof(SCONFIG_LIST, ConfigPtr) >= 4, "ConfigPtr offset");

int main(void) { return 0; }
EOF

if run_logged "$out_abs/logs/abi-native.log" \
        "$cc" -std=gnu11 -I"$repo/include" -fsyntax-only "$abi_dir/abi_probe.c"; then
    pass "native fixed-width ABI assertions ($cc)"
else
    fail "native fixed-width ABI assertions (see $out_abs/logs/abi-native.log)"
fi

required_exports=(
    PassThruOpen PassThruClose PassThruConnect PassThruDisconnect
    PassThruReadMsgs PassThruWriteMsgs PassThruStartPeriodicMsg
    PassThruStopPeriodicMsg PassThruStartMsgFilter PassThruStopMsgFilter
    PassThruSetProgrammingVoltage PassThruReadVersion PassThruGetLastError
    PassThruIoctl
)

if [[ $native_step_ok == 0 || -z $native_lib || ! -f $native_lib ]]; then
    skip "native export set: no native shared library was built"
elif ! command -v "$nm_bin" >/dev/null 2>&1; then
    skip "native export set: '$nm_bin' is not on PATH"
else
    if [[ $host_class == mac ]]; then
        nm_args=(-gU)
    else
        nm_args=(-D --defined-only)
    fi
    found=()
    while IFS= read -r symbol; do
        found+=("$symbol")
    done < <("$nm_bin" "${nm_args[@]}" "$native_lib" 2>/dev/null \
        | awk '{print $NF}' | grep '^_*PassThru' | sed 's/^_//' | sort -u)
    missing=()
    unexpected=()
    if ((${#found[@]})); then
        for name in "${found[@]}"; do
            printf '%s\n' "${required_exports[@]}" | grep -qx "$name" || unexpected+=("$name")
        done
        for name in "${required_exports[@]}"; do
            printf '%s\n' "${found[@]}" | grep -qx "$name" || missing+=("$name")
        done
    else
        missing=("${required_exports[@]}")
    fi
    if ((${#missing[@]})); then
        fail "native exports missing: ${missing[*]}"
    elif ((${#unexpected[@]})); then
        fail "native exports outside the J2534 set: ${unexpected[*]}"
    else
        pass "native export set: exactly the 14 J2534 exports"
    fi
fi

# --------------------------------------------------------------------------
section "Windows cross-build and exports"
# --------------------------------------------------------------------------
if [[ $want_mingw == 0 ]]; then
    skip "Windows cross-build disabled by --no-mingw"
else
    for arch in 32 64; do
        if [[ $arch == 32 ]]; then
            triple=i686-w64-mingw32
        else
            triple=x86_64-w64-mingw32
        fi
        mingw_cc="$triple-gcc"
        mingw_objdump="$triple-objdump"
        win_dir="$out_abs/windows-$arch"
        dll="$win_dir/MVCI$arch.dll"

        if ! command -v "$mingw_cc" >/dev/null 2>&1; then
            skip "Windows x$arch DLL: $mingw_cc not found (no MinGW compiler for this architecture)"
            continue
        fi
        mkdir -p "$win_dir"

        if ! run_logged "$out_abs/logs/windows-$arch.log" \
                "$mingw_cc" -std=gnu11 -O2 -Wall -Wextra -DMVCI_BUILDING_DLL \
                -I"$repo/include" -I"$repo/src" -shared \
                "$repo/src/passthru.c" "$repo/src/serial.c" "$repo/src/io.c" \
                "$repo/src/des.c" "$repo/src/mvci.def" -lbcrypt -o "$dll"; then
            fail "Windows x$arch DLL build (see $out_abs/logs/windows-$arch.log)"
            continue
        fi
        pass "Windows x$arch DLL build ($mingw_cc)"

        # Pointer-width and fixed-width ABI assertions for this target.
        cat >"$win_dir/abi_probe.c" <<'EOF'
#include <mvci/j2534.h>
#include <stddef.h>
_Static_assert(sizeof(J2534_ULONG) == 4, "J2534_ULONG");
_Static_assert(sizeof(void *) == 4 || sizeof(void *) == 8, "pointer width");
_Static_assert(sizeof(PASSTHRU_MSG) == 4152, "PASSTHRU_MSG");
#if defined(__i386__)
_Static_assert(sizeof(void *) == 4, "x86 pointer width");
_Static_assert(sizeof(SCONFIG_LIST) == 8, "x86 SCONFIG_LIST");
#elif defined(__x86_64__)
_Static_assert(sizeof(void *) == 8, "x64 pointer width");
_Static_assert(sizeof(SCONFIG_LIST) == 16, "x64 SCONFIG_LIST");
#endif
int main(void) { return 0; }
EOF
        if run_logged "$out_abs/logs/windows-$arch-abi.log" \
                "$mingw_cc" -std=gnu11 -I"$repo/include" -fsyntax-only "$win_dir/abi_probe.c"; then
            pass "Windows x$arch fixed-width ABI assertions"
        else
            fail "Windows x$arch fixed-width ABI assertions (see $out_abs/logs/windows-$arch-abi.log)"
        fi

        if ! command -v "$mingw_objdump" >/dev/null 2>&1; then
            skip "Windows x$arch exports: $mingw_objdump not found"
            continue
        fi
        win_names=()
        while IFS= read -r symbol; do
            win_names+=("$symbol")
        done < <("$mingw_objdump" -p "$dll" 2>/dev/null \
            | awk '/\[Ordinal\/Name Pointer\] Table/{f=1; next} f && /^$/{f=0} f{print $NF}' \
            | grep '^PassThru' | sort -u)
        win_missing=()
        win_unexpected=()
        decorated=0
        if ((${#win_names[@]})); then
            for name in "${required_exports[@]}"; do
                printf '%s\n' "${win_names[@]}" | grep -qx "$name" || win_missing+=("$name")
            done
            for name in "${win_names[@]}"; do
                base=${name%@*}
                [[ $name == *@* ]] && decorated=$((decorated + 1))
                printf '%s\n' "${required_exports[@]}" | grep -qx "$base" || win_unexpected+=("$name")
            done
        else
            win_missing=("${required_exports[@]}")
        fi
        if ((${#win_missing[@]})); then
            fail "Windows x$arch exports missing: ${win_missing[*]}"
        elif ((${#win_unexpected[@]})); then
            fail "Windows x$arch exports outside the J2534 set: ${win_unexpected[*]}"
        else
            pass "Windows x$arch export set: 14 undecorated J2534 exports"
            note "$decorated stdcall alias exports present; the undecorated names above are the contract"
        fi
    done
fi

if [[ $host_class != mac ]]; then
    skip "macOS library build: no Darwin compiler or sysroot on this host; the CommonCrypto path is compiled only on macOS"
fi

# --------------------------------------------------------------------------
section "Sanitizer build (ASAN + UBSAN)"
# --------------------------------------------------------------------------
san_dir="$out_abs/sanitize"
if [[ $want_sanitize == 0 ]]; then
    skip "sanitizer build disabled by --no-sanitize"
else
    mkdir -p "$san_dir"
    printf 'int main(void) { return 0; }\n' >"$san_dir/probe.c"
    if ! "$cc" -fsanitize=address,undefined "$san_dir/probe.c" -o "$san_dir/probe" \
            >"$out_abs/logs/sanitize-probe.log" 2>&1; then
        if sanitizer_runtime_missing "$out_abs/logs/sanitize-probe.log"; then
            skip "sanitizer build: the $cc sanitizer runtime is absent ($(first_reason "$out_abs/logs/sanitize-probe.log")); no test ran under ASAN/UBSAN"
        else
            fail "sanitizer probe build failed for a reason other than a missing runtime: $(first_reason "$out_abs/logs/sanitize-probe.log")"
        fi
    else
        san_flags=("${cflags[@]}" -O1 -g -fsanitize=address,undefined
                  -fno-sanitize-recover=undefined -fno-omit-frame-pointer)

        # san_build <name> <sources...>
        san_build() {
            local name=$1
            shift
            local def_args=() inc_args=() wrap_args=() word
            for word in "${def_words[@]}"; do def_args+=("-D$word"); done
            for word in "${inc_words[@]}"; do inc_args+=("-I$repo/$word"); done
            for word in "${wrap_words[@]}"; do wrap_args+=("-Wl,--wrap=$word"); done
            run_logged "$out_abs/logs/sanitize-$name-build.log" \
                "$cc" "${san_flags[@]}" "${def_args[@]}" "${inc_args[@]}" \
                "${wrap_args[@]}" "$@" "${link_flags[@]}" -o "$san_dir/$name"
        }

        for line in "${manifest_lines[@]}"; do
            IFS='|' read -r m_name m_ctest m_sources m_defines m_includes m_wraps m_hosts m_note <<<"$line"
            host_supported "$m_hosts" || continue
            split_field "$m_sources" source_words
            split_field "$m_defines" def_words
            split_field "$m_includes" inc_words
            split_field "$m_wraps" wrap_words
            source_paths=()
            for word in "${source_words[@]}"; do source_paths+=("$repo/$word"); done
            if san_build "$m_name" "${source_paths[@]}"; then
                if run_logged "$out_abs/logs/sanitize-$m_name.log" "$san_dir/$m_name"; then
                    pass "sanitizer $m_name (no arguments)"
                else
                    fail "sanitizer $m_name (no arguments)"
                fi
            else
                fail "sanitizer $m_name build"
            fi
        done
    fi
fi

# --------------------------------------------------------------------------
section "Summary"
# --------------------------------------------------------------------------
printf '  checks passed:  %d\n' "$pass_n"
printf '  checks failed:  %d\n' "$fail_n"
printf '  checks skipped: %d\n' "$skip_n"
if ((fail_n)); then
    printf 'Result: FAIL (see the failure reasons above)\n'
    exit 1
fi
if ((pass_n == 0)); then
    printf 'Result: FAIL (no check ran)\n'
    exit 1
fi
printf 'Result: PASS (offline only; physical timing and adapter behaviour remain unproven)\n'
exit 0
