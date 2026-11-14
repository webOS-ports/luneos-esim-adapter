#!/bin/sh
# @@@LICENSE
#
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# LICENSE@@@
#
# run.sh [workdir]
#
# Builds the test (test.c, shim.c and fake_ofono.c around the real
# src/esim_service.c, lpac_client.c and luna_service_utils.c) and runs it
# under:
#   ASan + UBSan (gcc and clang)   use after free, double free, overflows,
#                                  leaks (LeakSanitizer), undefined behaviour
#   TSan (gcc and clang)           races with the fake ofono's thread and
#                                  GDBus's own
#   valgrind memcheck              leaks and invalid accesses, uninstrumented
#
# Needs the development files of glib/gio, pbnjson_c and luna-service2 (its
# headers only: shim.c stands in for the library), and dbus-daemon for the
# private bus. These come from the build host by default. To build against
# a Yocto sysroot instead, set:
#   CC, CLANG     the compilers, with --sysroot and whatever else they need
#   PKG_CONFIG    a pkg-config that answers for the sysroot
#   RUN           how to start a binary built for it, e.g.
#                 "$SYSROOT/usr/lib/ld-linux-x86-64.so.2 --library-path
#                  $SYSROOT/usr/lib"
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)/src
WORK=${1:-${TMPDIR:-/tmp}/luneos-esim-adapter-tests}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
CC=${CC:-gcc}
CLANG=${CLANG:-clang}
PKG_CONFIG=${PKG_CONFIG:-pkg-config}
RUN=${RUN:-}

PKGS=$($PKG_CONFIG --cflags --libs glib-2.0 gio-2.0 gobject-2.0 pbnjson_c) || exit 1
LS2=$($PKG_CONFIG --cflags luna-service2) || exit 1
COMMON="-g -O1 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
	-Wno-missing-field-initializers -Werror -I$SRC -I$HERE"
SOURCES="$HERE/test.c $HERE/shim.c $HERE/fake_ofono.c $SRC/esim_service.c \
	$SRC/lpac_client.c $SRC/luna_service_utils.c"
# TSan maps its shadow at fixed addresses, which a kernel randomising more
# bits of the address space than it expects collides with
NORAND=
if command -v setarch >/dev/null 2>&1; then
	NORAND="setarch $(uname -m) -R"
fi
fail=0

build() {
	name=$1; cc=$2; shift 2
	echo "== build $name"
	# shellcheck disable=SC2086
	$cc $COMMON "$@" $LS2 $SOURCES $PKGS -o "$WORK/test-$name" \
		>"$WORK/$name-build.log" 2>&1 || {
		cat "$WORK/$name-build.log"; fail=1; return 1; }
}

run() {
	name=$1; shift
	# shellcheck disable=SC2086
	if TMPDIR=$WORK "$@" $RUN "$WORK/test-$name" --tap >"$WORK/$name.log" 2>&1; then
		echo "PASS $name ($(grep -c '^ok' "$WORK/$name.log") tests)"
	else
		echo "FAIL $name"; tail -40 "$WORK/$name.log"; fail=1
	fi
}

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:suppressions=$HERE/tsan.supp"
export G_SLICE=always-malloc G_DEBUG=gc-friendly
# The symbolizer otherwise reaches for debuginfod and can crash on its cache
export DEBUGINFOD_URLS=

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
build asan-gcc "$CC" $SAN && run asan-gcc
build asan-clang "$CLANG" $SAN && run asan-clang
build tsan-gcc "$CC" -fsanitize=thread && run tsan-gcc $NORAND
build tsan-clang "$CLANG" -fsanitize=thread && run tsan-clang $NORAND
build plain "$CC" && {
	if command -v valgrind >/dev/null 2>&1; then
		run plain valgrind --error-exitcode=99 --leak-check=full \
			--show-leak-kinds=definite --errors-for-leak-kinds=definite \
			--quiet
	else
		echo "valgrind not installed; skipped"
	fi
}

exit $fail
