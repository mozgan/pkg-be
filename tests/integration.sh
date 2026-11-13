#!/bin/sh
# Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
# Integration test for pkg-be on a live ZFS system. Needs root.
# It force-reinstalls one small package 3 times and checks that
# pkg-be created BEs and pruned them down to BE_KEEP.
#
#   doas sh tests/integration.sh            (default package: pkgconf)
#   doas env ITEST_PKG=tmux sh tests/integration.sh
set -eu

PKGNAME=${ITEST_PKG:-pkgconf}
# Removed and reinstalled to test the deinstall hook. Must be safe to remove.
DELPKG=${ITEST_DEL_PKG:-universal-ctags}
PREFIX=pkgbe-itest
KEEP=2
CONFDIR=$(pkg config PLUGINS_CONF_DIR)
CONF=${CONFDIR%/}/pkg-be.conf
BACKUP=$(mktemp -t pkgbe-itest)
fail=0

[ "$(id -u)" -eq 0 ] || { echo "SKIP: run as root"; exit 77; }
bectl list >/dev/null 2>&1 || { echo "SKIP: no ZFS boot environments"; exit 77; }
if pkg plugins | grep -q '^(null) '; then
	echo "FAIL: pkg $(pkg -v) does not keep plugin names, so pkg-be.conf"
	echo "      is not read (pkg 2.9.0 bug). Use pkg 2.8.x or 2.9.1+."
	exit 1
fi
pkg plugins | grep -q '^pkg-be ' || { echo "FAIL: pkg-be plugin not loaded"; exit 1; }
pkg info -e "$PKGNAME" || { echo "SKIP: $PKGNAME not installed"; exit 77; }

ours() { bectl list -H | awk -v p="$PREFIX-" 'index($1, p) == 1 { print $1 }'; }
# BEs that exist before the test are never touched by cleanup.
PRE_EXISTING=$(bectl list -H | awk '{print $1}')
cleanup() {
	for b in $(ours); do bectl destroy -o "$b" || true; done
	# A broken pkg-be.conf makes pkg use the default prefix "pkg-be".
	for b in $(bectl list -H | awk '$1 ~ /^pkg-be-[0-9]{8}-[0-9]{6}/ {print $1}'); do
		echo "$PRE_EXISTING" | grep -qx "$b" && continue
		echo "cleanup: $b was made with the default prefix (pkg-be.conf not read?)"
		bectl destroy -o "$b" || true
	done
	# cat, not cp: keep a world-readable 0644 file, not mktemp's 0600.
	if [ -s "$BACKUP" ]; then cat "$BACKUP" > "$CONF"; chmod 0644 "$CONF"; else rm -f "$CONF"; fi
	rm -f "$BACKUP"
}
[ -f "$CONF" ] && cp "$CONF" "$BACKUP"
trap cleanup EXIT INT TERM

printf 'BE_ENABLED = true;\nBE_PREFIX = "%s";\nBE_KEEP = %d;\n' "$PREFIX" "$KEEP" > "$CONF"

# check DESC COND [EXPLAIN]: on failure EXPLAIN (a shell snippet) prints
# what was expected and what happened instead.
check() {
	if eval "$2"; then echo "ok   - $1"; return; fi
	echo "FAIL - $1"; fail=1
	[ $# -ge 3 ] && eval "$3" | sed 's/^/       /'
	return 0
}

# 1) One upgrade -> one BE.
pkg upgrade -fy "$PKGNAME" >/dev/null
check "first upgrade creates a BE" '[ "$(ours | wc -l)" -eq 1 ]'
DS="$(zfs list -H -o name / | sed 's,/[^/]*$,,')/$(ours | head -1)"
check "metadata: op=upgrade" '[ "$(zfs get -H -o value pkg-be:op "$DS")" = upgrade ]'
check "metadata: packages mentions $PKGNAME" 'zfs get -H -o value pkg-be:packages "$DS" | grep -q "^$PKGNAME "'
check "metadata: count=1" '[ "$(zfs get -H -o value pkg-be:count "$DS")" = 1 ]'

# 2) Two more (same second possible) -> pruned to KEEP, unique names.
pkg upgrade -fy "$PKGNAME" >/dev/null
pkg upgrade -fy "$PKGNAME" >/dev/null
check "retention keeps $KEEP" '[ "$(ours | wc -l)" -eq '"$KEEP"' ]'

# 3) Foreign BEs untouched.
check "default BE still exists" 'bectl list -H | awk "{print \$1}" | grep -qx default'

# 4) Disabled -> no BE.
before=$(ours | wc -l)
printf 'BE_ENABLED = false;\nBE_PREFIX = "%s";\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_ENABLED=false creates nothing" '[ "$(ours | wc -l)" -eq '"$before"' ]'

# 5) Dry run -> no BE.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
pkg upgrade -nf "$PKGNAME" >/dev/null || true
check "dry run (-n) creates nothing" '[ "$(ours | wc -l)" -eq '"$before"' ]'

# 6) BE_HOOKS: default (upgrade only) -> install makes no BE.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
pkg install -fy "$PKGNAME" >/dev/null
check "install without hook creates nothing" '[ "$(ours | wc -l)" -eq '"$before"' ]'

# 7) BE_HOOKS = install -> install makes a BE, upgrade does not.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_HOOKS = "install";\n' "$PREFIX" > "$CONF"
pkg install -fy "$PKGNAME" >/dev/null
check "BE_HOOKS=install: install creates a BE" '[ "$(ours | wc -l)" -eq $((before + 1)) ]'
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_HOOKS=install: upgrade creates nothing" '[ "$(ours | wc -l)" -eq $((before + 1)) ]'

# 8) BE_HOOKS = deinstall -> delete makes a BE.
if pkg info -e "$DELPKG"; then
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_HOOKS = "deinstall";\n' "$PREFIX" > "$CONF"
	pkg delete -y "$DELPKG" >/dev/null
	check "BE_HOOKS=deinstall: delete creates a BE" '[ "$(ours | wc -l)" -eq $((before + 2)) ]'
	rm -f "$CONF"	# reinstall without the plugin config
	pkg install -y "$DELPKG" >/dev/null
else
	echo "skip - $DELPKG not installed, deinstall hook not tested"
fi

# 9) "pkg be" CLI: list shows our BE, diff sees a package added after it.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
last=$(ours | tail -1)
check "pkg be list shows $last" 'pkg be list | grep -q "^$last "'
check "pkg be show prints op" 'pkg be show "$last" | grep -q "^Operation: upgrade"'
if pkg info -e "$DELPKG"; then
	pkg delete -y "$DELPKG" >/dev/null
	check "pkg be diff sees removed $DELPKG" 'pkg be diff "$last" | grep -q "^  -$DELPKG-"'
	pkg install -y "$DELPKG" >/dev/null
fi
check "pkg be diff refuses running BE" '! pkg be diff "$(bectl list -H | awk "\$2 ~ /N/ {print \$1}")" 2>/dev/null'
check "pkg be prune -n lists nothing with BE_KEEP=0" 'pkg be prune -n | grep -q "are 0: nothing"'

# 10) "pkg -r DIR": the BE would be of the wrong system -> must skip.
ROOTDIR=$(mktemp -d -t pkgbe-root)
# "pkg -r DIR" loads plugins and pkg.conf from DIR, so copy them there too.
tar -C / -cf - etc/pkg usr/local/etc/pkg usr/local/etc/pkg.conf usr/local/lib/pkg \
    usr/share/keys var/db/pkg 2>/dev/null | tar -C "$ROOTDIR" -xf -
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_HOOKS = "all";\n' "$PREFIX" > "$CONF"
before=$(ours | wc -l)
out=$(pkg -r "$ROOTDIR" upgrade -fy "$PKGNAME" 2>&1 || true)
check "pkg -r: no BE" '[ "$(ours | wc -l)" -eq '"$before"' ]'
check "pkg -r: says why" 'echo "$out" | grep -q "another root"'
rm -rf "$ROOTDIR"

# 11) Inside a jail -> skip.
out=$(jail -c path=/ name=pkgbe_itest host.hostname=pkgbe-itest ip4=inherit \
    command=/usr/local/sbin/pkg upgrade -fy "$PKGNAME" 2>&1 || true)
check "jail: no BE" '[ "$(ours | wc -l)" -eq '"$before"' ]'
check "jail: says why" 'echo "$out" | grep -q "running in a jail"'

# 12) syslog.
pkg upgrade -fy "$PKGNAME" >/dev/null
last=$(ours | tail -1)
sleep 1
check "syslog has the new BE" 'grep "pkg-be: " /var/log/messages | grep -q "$last"' \
    'echo "want a \"pkg-be: ... $last\" line; last pkg-be lines:"; grep "pkg-be" /var/log/messages | tail -3'
# v0.2.0: no openlog() any more, so the ident is pkg's, not "pkg-be[pid]".
check "syslog ident is pkg's own" '! grep "$last" /var/log/messages | grep -q " pkg-be\["' \
    'echo "want ident pkg[...], got:"; grep "$last" /var/log/messages | tail -2'

# 12b) pkg.conf SYSLOG = false silences pkg-be too (BE_SYSLOG stays true).
pkg -o SYSLOG=false upgrade -fy "$PKGNAME" >/dev/null
last=$(ours | tail -1)
sleep 1
check "SYSLOG=false: BE created" 'bectl list -H | grep -q "^$last"'
check "SYSLOG=false: nothing logged" '! grep -q "$last" /var/log/messages' \
    'echo "want no line with $last, got:"; grep "$last" /var/log/messages'

# 13) BE_KEEP_DAYS: a BE named as if from 2020 is destroyed, foreign ones stay.
bectl create "$PREFIX-20200101-000000"
bectl create pkgbe-itest-manual
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_KEEP_DAYS = 30;\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_KEEP_DAYS destroys old BE" '! bectl list -H | grep -q "^$PREFIX-20200101-000000"'
check "BE_KEEP_DAYS keeps recent BEs" '[ "$(ours | wc -l)" -ge 2 ]'
check "foreign BE kept" 'bectl list -H | grep -q "^pkgbe-itest-manual"'
bectl destroy -o pkgbe-itest-manual

# 14) BE_MIN_FREE = 100 -> warning (the pool is never 100% free).
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_MIN_FREE = 100;\n' "$PREFIX" > "$CONF"
out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
check "BE_MIN_FREE warns" 'echo "$out" | grep -q "free in the pool"'

# 15) diff between two BEs.
if pkg info -e "$DELPKG"; then
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
	pkg upgrade -fy "$PKGNAME" >/dev/null; a=$(ours | tail -1)
	pkg delete -y "$DELPKG" >/dev/null
	pkg upgrade -fy "$PKGNAME" >/dev/null; b=$(ours | tail -1)
	pkg install -y "$DELPKG" >/dev/null
	check "pkg be diff A B sees removal" 'pkg be diff "$a" "$b" | grep -q "^  -$DELPKG-"'
	check "pkg be diff A A refused" '! pkg be diff "$a" "$a" 2>/dev/null'
fi

# ---- v0.2.0 ----
REPO=$(pkg query %R "$PKGNAME")

# 16) BE_REPOSITORIES: the package's own repository -> BE.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_REPOSITORIES = "%s";\n' "$PREFIX" "$REPO" > "$CONF"
before=$(ours | wc -l)
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_REPOSITORIES=$REPO: BE created" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
    'echo "want $((before + 1)) BEs, got $(ours | wc -l)"'

# 17) Unlisted repositories, a near-miss name and wrong case -> no BE.
for f in "no-such-repo" "${REPO%?}" "$(echo "$REPO" | tr '[:upper:]' '[:lower:]')" "local"; do
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_REPOSITORIES = "%s";\n' "$PREFIX" "$f" > "$CONF"
	before=$(ours | wc -l)
	out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
	check "BE_REPOSITORIES=\"$f\" ($PKGNAME is from $REPO): no BE" '[ "$(ours | wc -l)" -eq '"$before"' ]' \
	    'echo "want $before BEs, got $(ours | wc -l)"'
	check "BE_REPOSITORIES=\"$f\": says why" 'echo "$out" | grep -q "no package from BE_REPOSITORIES"' \
	    'echo "want \"no package from BE_REPOSITORIES\", output was:"; echo "$out" | tail -5'
done

# 18) A list where only one item matches, with odd separators -> BE.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_REPOSITORIES = " no-such ,, %s\\t";\n' "$PREFIX" "$REPO" > "$CONF"
before=$(ours | wc -l)
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_REPOSITORIES list with blanks: BE created" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
    'echo "want $((before + 1)) BEs, got $(ours | wc -l)"'

# 19) Deinstall: installed packages keep their repository name.
if pkg info -e "$DELPKG"; then
	DREPO=$(pkg query %R "$DELPKG")
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_HOOKS = "deinstall";\nBE_REPOSITORIES = "%s";\n' "$PREFIX" "$DREPO" > "$CONF"
	before=$(ours | wc -l)
	pkg delete -y "$DELPKG" >/dev/null
	check "BE_REPOSITORIES=$DREPO on delete: BE created" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
	    'echo "want $((before + 1)) BEs, got $(ours | wc -l); repo of $DELPKG was \"$DREPO\""'
	rm -f "$CONF"
	pkg install -y "$DELPKG" >/dev/null
fi

# 20) BE_KEEP_AGE: same as BE_KEEP_DAYS, in the new form.
bectl create "$PREFIX-20200101-000000"
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_KEEP_AGE = "30d";\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_KEEP_AGE=30d destroys a 2020 BE" '! bectl list -H | grep -q "^$PREFIX-20200101-000000"' \
    'echo "want it gone, still there"'
check "BE_KEEP_AGE=30d keeps today's BEs" '[ "$(ours | wc -l)" -ge 2 ]' \
    'echo "want >= 2, got $(ours | wc -l)"'

# 21) BE_MIN_AGE protects a young BE that BE_KEEP = 1 would destroy.
young="$PREFIX-$(date -v-10M +%Y%m%d-%H%M%S)"
old="$PREFIX-20200101-000000"
bectl create "$old"; bectl create "$young"
printf 'BE_PREFIX = "%s";\nBE_KEEP = 1;\nBE_MIN_AGE = "1h";\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_MIN_AGE=1h keeps a 10-minute-old BE" 'bectl list -H | grep -q "^$young"' \
    'echo "want $young kept (BE_KEEP=1 alone would destroy it); BEs now:"; ours'
check "BE_MIN_AGE=1h still destroys the 2020 BE" '! bectl list -H | grep -q "^$old"' \
    'echo "want $old destroyed; BEs now:"; ours'
check "BE_MIN_AGE: more than BE_KEEP remain" '[ "$(ours | wc -l)" -gt 1 ]'

# 22) Without BE_MIN_AGE the same young BE goes (proves 21 was the rule).
printf 'BE_PREFIX = "%s";\nBE_KEEP = 1;\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_KEEP=1 alone: young BE destroyed" '! bectl list -H | grep -q "^$young"' \
    'echo "want $young gone; BEs now:"; ours'
check "BE_KEEP=1 alone: exactly 1 left" '[ "$(ours | wc -l)" -eq 1 ]' \
    'echo "want 1, got $(ours | wc -l):"; ours'

# 23) Invalid durations: a BE is still made, nothing is destroyed.
for bad in 'BE_MIN_AGE = "abc"' 'BE_KEEP_AGE = "-1d"' 'BE_KEEP_AGE = "1.5d"' \
    'BE_MIN_AGE = "99999999999"' 'BE_KEEP_DAYS = 99999'; do
	bectl create "$old"
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 1;\n%s;\n' "$PREFIX" "$bad" > "$CONF"
	before=$(ours | wc -l)
	out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
	check "$bad: BE still created" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
	    'echo "want $((before + 1)) BEs (create, never prune), got $(ours | wc -l)"'
	check "$bad: nothing pruned" 'bectl list -H | grep -q "^$old"' \
	    'echo "want $old kept on a bad config; it was destroyed"'
	check "$bad: error shown" 'echo "$out" | grep -q "invalid BE_"' \
	    'echo "want \"invalid BE_...\", output was:"; echo "$out" | tail -5'
	rc=0; pkg be prune -n >/dev/null 2>&1 || rc=$?
	check "$bad: pkg be prune exits 78 (EX_CONFIG)" '[ $rc -eq 78 ]' 'echo "got exit $rc"'
	bectl destroy -o "$old"
done

# 24) Durations must be quoted. pkg drops an unquoted one with a warning,
#     the default "0" (off) stays: nothing is destroyed by mistake.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_AGE = 7200;\nBE_MIN_AGE = 60;\n' "$PREFIX" > "$CONF"
out=$(pkg be prune -n 2>&1)
check "unquoted duration: pkg warns" 'echo "$out" | grep -q "Malformed key BE_KEEP_AGE"' \
    'echo "want pkg\x27s \"Malformed key BE_KEEP_AGE\" warning, got: $out"'
check "unquoted duration: stays 0 (off)" 'echo "$out" | grep -q "BE_KEEP_AGE = 0s, BE_MIN_AGE = 0s"' \
    'echo "want 0s/0s, got: $out"'
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_AGE = "7200";\nBE_MIN_AGE = "60";\n' "$PREFIX" > "$CONF"
out=$(pkg be prune -n 2>&1)
check "quoted number = seconds" 'echo "$out" | grep -q "BE_KEEP_AGE = 7200s, BE_MIN_AGE = 60s"' \
    'echo "want 7200s/60s, got: $out"'
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_AGE = " 2w ";\nBE_MIN_AGE = "0";\n' "$PREFIX" > "$CONF"
out=$(pkg be prune -n 2>&1)
check "\" 2w \" read as 1209600s" 'echo "$out" | grep -q "BE_KEEP_AGE = 1209600s"' \
    'echo "want 1209600s, got: $out"'

# 25) BE_KEEP_AGE wins over BE_KEEP_DAYS when both are set.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_DAYS = 3;\nBE_KEEP_AGE = "1h";\n' "$PREFIX" > "$CONF"
out=$(pkg be prune -n 2>&1)
check "BE_KEEP_AGE beats BE_KEEP_DAYS" 'echo "$out" | grep -q "BE_KEEP_AGE = 3600s"' \
    'echo "want 3600s (1h), got: $out"'

[ $fail -eq 0 ] && echo "PASS" || echo "FAILED"
exit $fail
