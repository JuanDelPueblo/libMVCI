#!/usr/bin/env bash
# check-offline.sh - hardware-free build, ABI, export and test verification.
#
# Runs only offline work. It never opens a serial port, loads a provider,
# contacts an adapter or a vehicle, installs a package, or uses the network.
# Every artifact goes to one isolated output directory.
#
# Steps:
#   1. Native build. Uses CMake when it is available; otherwise compiles the
#      same source set directly with the native C compiler.
#   2. Native tests with no arguments: the codec self-test, the provider
#      sequence test, and any standalone test/mvci_*_test.c test.
#   3. Fixed-width J2534 ABI assertions and the native export set.
#   4. Optional MinGW x86/x64 DLL build and undecorated-export check.
#   5. Optional ASAN/UBSAN build and run.
#
# A missing prerequisite is reported as SKIP with the reason. A compile or
# test that starts and fails is reported as FAIL and exits non-zero; it is
# never converted into a skip. The undocumented live-test argument is never
# passed.
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
# Environment:
#   MVCI_OFFLINE_TESTS_NO_MOCK
#       Space- or semicolon-separated test names whose test source supplies
#       its own mvci_io_* stubs instead of linking test/mock_io.c. Keep this
#       equal to the CMake list of the same name when both paths are used.
#
# Exit status: 0 when every executed check passed and at least one skip or
# pass exists. 1 when any check failed. 2 for a usage or compiler error.

set -uo pipefail

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

while (($#)); do
    case "$1" in
        --out-dir) out_dir=${2-}; shift 2 ;;
        --cc) cc=${2-}; shift 2 ;;
        --cmake) cmake_bin=${2-}; shift 2 ;;
        --no-cmake) want_cmake=0; shift ;;
        --no-mingw) want_mingw=0; shift ;;
        --no-sanitize) want_sanitize=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z $out_dir ]]; then
    printf 'error: --out-dir needs a value\n' >&2
    exit 2
fi
if ! command -v "$cc" >/dev/null 2>&1; then
    printf 'FAIL  native C compiler not found: %s\n' "$cc" >&2
    exit 2
fi

ctest_bin=""
if command -v "$cmake_bin" >/dev/null 2>&1; then
    ctest_bin="$(dirname "$(command -v "$cmake_bin")")/ctest"
    if [[ ! -x $ctest_bin ]]; then
        ctest_bin=$(command -v ctest || true)
    fi
fi

case "$out_dir" in
    /*) ;;
    *) out_dir="$PWD/$out_dir" ;;
esac
mkdir -p "$out_dir" || exit 2
rm -rf "$out_dir/logs"
mkdir -p "$out_dir/logs" || exit 2

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

cflags=(-std=gnu11 -O2 -Wall -Wextra -Wno-deprecated-declarations
        -I"$repo/include" -I"$repo/src")

link_flags=(-pthread)
if [[ $(uname -s) != Darwin ]]; then
    link_flags=(-lcrypto "${link_flags[@]}")
fi

no_mock=" ${MVCI_OFFLINE_TESTS_NO_MOCK:-} "
no_mock=${no_mock//;/ }
is_no_mock() { [[ $no_mock == *" $1 "* ]]; }
no_mock_cmake=${no_mock// /;}
no_mock_cmake=${no_mock_cmake#;}
no_mock_cmake=${no_mock_cmake%;}

# A standalone test is test/mvci_<area>_test.c other than the sequence test.
extra_tests=()
while IFS= read -r src; do
    [[ -n $src ]] || continue
    name=$(basename "$src" .c)
    [[ $name == mvci_seq_test ]] && continue
    extra_tests+=("$name")
done < <(find "$repo/test" -maxdepth 1 -type f -name 'mvci_*_test.c' -print | sort)

printf 'libMVCI offline check\n'
printf '  repo:      %s\n' "$repo"
printf '  output:    %s\n' "$out_dir"
printf '  compiler:  %s (%s)\n' "$(command -v "$cc")" "$("$cc" --version 2>/dev/null | head -1)"
if command -v "$cmake_bin" >/dev/null 2>&1; then
    printf '  cmake:     %s\n' "$("$cmake_bin" --version 2>/dev/null | head -1)"
else
    printf '  cmake:     not found (%s)\n' "$cmake_bin"
fi
printf '  extra tests: '
if ((${#extra_tests[@]})); then
    printf '%s\n' "${extra_tests[*]}"
else
    printf '(none beyond the codec and sequence tests)\n'
fi

# --------------------------------------------------------------------------
section "Native build and hardware-free tests"
# --------------------------------------------------------------------------
native_dir="$out_dir/native"
rm -rf "$native_dir"
mkdir -p "$native_dir"
native_lib=""
native_step_ok=0

if [[ $want_cmake != 0 ]] && command -v "$cmake_bin" >/dev/null 2>&1; then
    cmake_dir="$out_dir/cmake-native"
    rm -rf "$cmake_dir"
    if run_logged "$out_dir/logs/cmake-configure.log" \
            "$cmake_bin" -S "$repo" -B "$cmake_dir" \
            -DCMAKE_BUILD_TYPE=Release -DMVCI_BUILD_TESTS=ON -DMVCI_INSTALL=OFF \
            -DMVCI_OFFLINE_TESTS_NO_MOCK="$no_mock_cmake" \
        && run_logged "$out_dir/logs/cmake-build.log" \
            "$cmake_bin" --build "$cmake_dir" --parallel; then
        native_lib=$(find "$cmake_dir" -maxdepth 2 -type f \
            \( -name 'libMVCI.so*' -o -name 'libMVCI*.dylib' \) | head -1)
        if [[ -z $native_lib ]]; then
            fail "CMake native build produced no libMVCI shared library"
        else
            native_step_ok=1
            if [[ -z $ctest_bin ]]; then
                skip "CTest not found next to $cmake_bin; the CMake build ran but no tests ran"
            elif run_logged "$out_dir/logs/ctest.log" \
                    "$ctest_bin" --test-dir "$cmake_dir" --output-on-failure; then
                pass "CMake build and CTest (no-argument tests)"
            else
                fail "CTest (see $out_dir/logs/ctest.log)"
            fi
            if grep -q 'No tests were found' "$out_dir/logs/ctest.log" 2>/dev/null; then
                fail "CTest discovered no tests to run"
            fi
        fi
    else
        fail "CMake configure/build (see $out_dir/logs)"
    fi
else
    if [[ $want_cmake == 0 ]]; then
        skip "CMake step disabled by --no-cmake; used the direct compiler path"
    else
        skip "CMake not found ('$cmake_bin' is not on PATH); used the direct compiler path"
    fi

    native_lib="$native_dir/libMVCI.so"
    [[ $(uname -s) == Darwin ]] && native_lib="$native_dir/libMVCI.dylib"

    if run_logged "$out_dir/logs/native-lib.log" \
            "$cc" "${cflags[@]}" -fPIC -fvisibility=hidden -DMVCI_BUILDING_DLL \
            -shared "$repo/src/passthru.c" "$repo/src/serial.c" \
            "$repo/src/io.c" "$repo/src/des.c" "${link_flags[@]}" \
            -o "$native_lib"; then
        pass "native shared library build ($cc)"
        native_step_ok=1
    else
        fail "native shared library build (see $out_dir/logs/native-lib.log)"
    fi

    # <name> <sources...> for one no-argument test binary.
    build_direct_test() {
        local name=$1
        shift
        run_logged "$out_dir/logs/$name-build.log" \
            "$cc" "${cflags[@]}" "$@" "${link_flags[@]}" -o "$native_dir/$name"
    }

    if build_direct_test mvci_test \
            "$repo/test/mvci_test.c" "$repo/src/serial.c" "$repo/src/io.c" "$repo/src/des.c"; then
        if run_logged "$out_dir/logs/mvci_test.log" "$native_dir/mvci_test"; then
            pass "mvci_test (codec self-test, no arguments)"
        else
            fail "mvci_test (no arguments)"
        fi
    else
        fail "mvci_test build"
    fi

    seq_sources=("$repo/test/mvci_seq_test.c" "$repo/test/mock_io.c"
                 "$repo/src/passthru.c" "$repo/src/serial.c" "$repo/src/des.c")
    if build_direct_test mvci_seq_test "${seq_sources[@]}"; then
        if run_logged "$out_dir/logs/mvci_seq_test.log" "$native_dir/mvci_seq_test"; then
            pass "mvci_seq_test (provider sequence, no arguments)"
        else
            fail "mvci_seq_test (no arguments)"
        fi
    else
        fail "mvci_seq_test build"
    fi

    if ((${#extra_tests[@]})); then
        for name in "${extra_tests[@]}"; do
            sources=("$repo/test/$name.c" "$repo/src/passthru.c"
                     "$repo/src/serial.c" "$repo/src/des.c")
            if ! is_no_mock "$name"; then
                sources+=("$repo/test/mock_io.c")
            fi
            [[ -f "$repo/test/${name}_support.c" ]] && sources+=("$repo/test/${name}_support.c")
            if build_direct_test "$name" "${sources[@]}"; then
                if run_logged "$out_dir/logs/$name.log" "$native_dir/$name"; then
                    pass "$name (no arguments)"
                else
                    fail "$name (no arguments)"
                fi
            else
                fail "$name build"
            fi
        done
    fi
fi

# --------------------------------------------------------------------------
section "Fixed-width J2534 ABI"
# --------------------------------------------------------------------------
abi_dir="$out_dir/abi"
rm -rf "$abi_dir"
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

if run_logged "$out_dir/logs/abi-native.log" \
        "$cc" -std=gnu11 -I"$repo/include" -fsyntax-only "$abi_dir/abi_probe.c"; then
    pass "native fixed-width ABI assertions ($cc)"
else
    fail "native fixed-width ABI assertions (see $out_dir/logs/abi-native.log)"
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
    if [[ $(uname -s) == Darwin ]]; then
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
        win_dir="$out_dir/windows-$arch"
        dll="$win_dir/MVCI$arch.dll"

        if ! command -v "$mingw_cc" >/dev/null 2>&1; then
            skip "Windows x$arch DLL: $mingw_cc not found (no MinGW compiler for this architecture)"
            continue
        fi
        rm -rf "$win_dir"
        mkdir -p "$win_dir"

        if ! run_logged "$out_dir/logs/windows-$arch.log" \
                "$mingw_cc" -std=gnu11 -O2 -Wall -Wextra -DMVCI_BUILDING_DLL \
                -I"$repo/include" -I"$repo/src" -shared \
                "$repo/src/passthru.c" "$repo/src/serial.c" "$repo/src/io.c" \
                "$repo/src/des.c" "$repo/src/mvci.def" -lbcrypt -o "$dll"; then
            fail "Windows x$arch DLL build (see $out_dir/logs/windows-$arch.log)"
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
        if run_logged "$out_dir/logs/windows-$arch-abi.log" \
                "$mingw_cc" -std=gnu11 -I"$repo/include" -fsyntax-only "$win_dir/abi_probe.c"; then
            pass "Windows x$arch fixed-width ABI assertions"
        else
            fail "Windows x$arch fixed-width ABI assertions (see $out_dir/logs/windows-$arch-abi.log)"
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

if [[ $(uname -s) != Darwin ]]; then
    skip "macOS backend: no Darwin compiler or sysroot on this host; the CommonCrypto path is compiled only on macOS"
fi

# --------------------------------------------------------------------------
section "Sanitizer build (ASAN + UBSAN)"
# --------------------------------------------------------------------------
san_dir="$out_dir/sanitize"
if [[ $want_sanitize == 0 ]]; then
    skip "sanitizer build disabled by --no-sanitize"
else
    rm -rf "$san_dir"
    mkdir -p "$san_dir"
    printf 'int main(void) { return 0; }\n' >"$san_dir/probe.c"
    if ! "$cc" -fsanitize=address,undefined "$san_dir/probe.c" -o "$san_dir/probe" \
            >"$out_dir/logs/sanitize-probe.log" 2>&1; then
        skip "sanitizer runtime unavailable: $cc cannot link -fsanitize=address,undefined ($(first_reason "$out_dir/logs/sanitize-probe.log"))"
    else
        san_flags=("${cflags[@]}" -O1 -g -fsanitize=address,undefined
                  -fno-sanitize-recover=undefined -fno-omit-frame-pointer)

        san_build() {
            local name=$1
            shift
            run_logged "$out_dir/logs/sanitize-$name-build.log" \
                "$cc" "${san_flags[@]}" "$@" "${link_flags[@]}" -o "$san_dir/$name"
        }
        san_run() {
            local name=$1
            if run_logged "$out_dir/logs/sanitize-$name.log" "$san_dir/$name"; then
                pass "sanitizer $name (no arguments)"
            else
                fail "sanitizer $name (no arguments)"
            fi
        }

        if san_build mvci_test "$repo/test/mvci_test.c" "$repo/src/serial.c" \
                "$repo/src/io.c" "$repo/src/des.c"; then
            san_run mvci_test
        else
            fail "sanitizer mvci_test build"
        fi

        if san_build mvci_seq_test "$repo/test/mvci_seq_test.c" "$repo/test/mock_io.c" \
                "$repo/src/passthru.c" "$repo/src/serial.c" "$repo/src/des.c"; then
            san_run mvci_seq_test
        else
            fail "sanitizer mvci_seq_test build"
        fi

        if ((${#extra_tests[@]})); then
            for name in "${extra_tests[@]}"; do
                sources=("$repo/test/$name.c" "$repo/src/passthru.c"
                         "$repo/src/serial.c" "$repo/src/des.c")
                if ! is_no_mock "$name"; then
                    sources+=("$repo/test/mock_io.c")
                fi
                [[ -f "$repo/test/${name}_support.c" ]] && sources+=("$repo/test/${name}_support.c")
                if san_build "$name" "${sources[@]}"; then
                    san_run "$name"
                else
                    fail "sanitizer $name build"
                fi
            done
        fi
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
printf 'Result: PASS (offline only; physical timing and adapter behaviour remain unproven)\n'
exit 0
