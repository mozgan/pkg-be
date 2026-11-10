#!/bin/sh
# Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Run every test case of an ATF-C program directly, without kyua.
# Used when kyua or its store schema is not installed.
#
#   sh run_atf.sh ./be_util_test
set -u

prog=$1
pass=0
fail=0
for tc in $("$prog" -l | sed -n 's/^ident: //p'); do
	if out=$("$prog" "$tc" 2>&1); then
		echo "$tc  ->  passed"
		pass=$((pass + 1))
	else
		echo "$tc  ->  FAILED"
		echo "$out" | sed 's/^/    /'
		fail=$((fail + 1))
	fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
