#!/bin/sh
# Copyright (c) 2026 M. Ozgan <mozgan@gmail.com>
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
SRC=$(cd "$(dirname "$0")/.." && pwd)
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
ROOTDS=$(zfs list -H -o name / | sed 's,/[^/]*$,,')
nsnap() { zfs list -H -t snapshot -o name -r "$ROOTDS" | wc -l; }
NSNAP0=$(nsnap)
IMMUT=""
CT=""; HTTPD=""; SO_BAK=""; DBDS=""; MNTBE=""; CHILD=""
cleanup() {
	if [ -n "$CHILD" ] && zfs list "$CHILD" >/dev/null 2>&1; then
		zfs destroy -r "$CHILD" || true
	fi
	if [ -n "$SO_BAK" ]; then cat "$SO_BAK" > "$SO"; rm -f "$SO_BAK"; fi
	if [ -n "$MNTBE" ]; then bectl umount "$MNTBE" 2>/dev/null || true; fi
	if [ -n "$DBDS" ]; then zfs destroy -r "$DBDS" || true; fi
	[ -n "$IMMUT" ] && chflags noschg "$IMMUT"
	[ -n "$HTTPD" ] && kill "$HTTPD" 2>/dev/null
	if [ -n "$CT" ]; then
		pkg delete -qy -g 'pkgbe-itest-c[12]' >/dev/null 2>&1 || true
		rm -rf "$CT" /usr/local/share/pkgbe-itest-conflict
	fi
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

# 13) BE_KEEP_DAYS (removed in 0.3.0): warned about, ignored, nothing pruned.
bectl create "$PREFIX-20200101-000000"
bectl create pkgbe-itest-manual
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_KEEP_DAYS = 30;\n' "$PREFIX" > "$CONF"
out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
check "BE_KEEP_DAYS: removal warned" 'echo "$out" | grep -q "BE_KEEP_DAYS was removed.*BE_KEEP_AGE = \"30d\""' \
    'echo "want the removal warning with BE_KEEP_AGE = \"30d\", output was:"; echo "$out" | tail -5'
check "BE_KEEP_DAYS: ignored, 2020 BE kept" 'bectl list -H | grep -q "^$PREFIX-20200101-000000"' \
    'echo "want it kept (the key does nothing now); it was destroyed"'
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_KEEP_AGE = "30d";\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_KEEP_AGE=30d destroys the 2020 BE" '! bectl list -H | grep -q "^$PREFIX-20200101-000000"'
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
    'BE_MIN_AGE = "99999999999"' 'BE_KEEP_AGE = "24856d"'; do
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

# 25) BE_KEEP_DAYS is not added to BE_KEEP_AGE any more.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_DAYS = 3;\nBE_KEEP_AGE = "1h";\n' "$PREFIX" > "$CONF"
out=$(pkg be prune -n 2>&1)
check "BE_KEEP_DAYS does not change BE_KEEP_AGE" 'echo "$out" | grep -q "BE_KEEP_AGE = 3600s"' \
    'echo "want 3600s (1h), got: $out"'
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_DAYS = 0;\n' "$PREFIX" > "$CONF"
out=$(pkg be prune -n 2>&1)
check "BE_KEEP_DAYS = 0: no warning" '! echo "$out" | grep -q "BE_KEEP_DAYS"' \
    'echo "want no warning for 0, got: $out"'

# ---- v0.3.0 ----

# 26) Success -> pkg-be:status = complete (set by the post hook).
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
last=$(ours | tail -1); DS="$(zfs list -H -o name / | sed 's,/[^/]*$,,')/$last"
check "status complete after success" '[ "$(zfs get -H -o value pkg-be:status "$DS")" = complete ]' \
    'echo "want complete, got \"$(zfs get -H -o value pkg-be:status "$DS")\""'
check "pkg be show prints status" 'pkg be show "$last" | grep -q "^Status: *complete"' \
    'pkg be show "$last" | head -6'

# 27) Failure -> status stays incomplete. An immutable directory makes the
#     extraction fail after the pre hook ran (an immutable file does not:
#     pkg still succeeds).
IMMUT=$(pkg info -l "$PKGNAME" | awk '/\/bin\//{print $1; exit}' | xargs dirname 2>/dev/null)
if [ -n "$IMMUT" ] && chflags schg "$IMMUT"; then
	before=$(ours | wc -l)
	pkg upgrade -fy "$PKGNAME" >/dev/null 2>&1 && rc=0 || rc=$?
	chflags noschg "$IMMUT"; IMMUT=""

	last=$(ours | tail -1); DS="$(zfs list -H -o name / | sed 's,/[^/]*$,,')/$last"
	check "failed pkg run: pkg exited non-zero" '[ $rc -ne 0 ]' 'echo "pkg exit was $rc; the failure was not provoked"'
	check "failed pkg run: BE made" '[ "$(ours | wc -l)" -eq $((before + 1)) ]'
	check "failed pkg run: status incomplete" '[ "$(zfs get -H -o value pkg-be:status "$DS")" = incomplete ]' \
	    'echo "want incomplete, got \"$(zfs get -H -o value pkg-be:status "$DS")\""'
	pkg upgrade -fy "$PKGNAME" >/dev/null	# repair
else
	echo "skip - no binary in $PKGNAME to make immutable"
fi

# 28) One pkg run, one BE: upgrade + autoremove runs two operations.
if pkg install -Ay "$DELPKG" >/dev/null 2>&1; then
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\nBE_HOOKS = "all";\n' "$PREFIX" > "$CONF"
	before=$(ours | wc -l)
	out=$(pkg upgrade -fy --autoremove "$PKGNAME" 2>&1)
	check "upgrade --autoremove: $DELPKG autoremoved" '! pkg info -e "$DELPKG"' 'echo "$DELPKG still installed; test is not valid"'
	check "upgrade --autoremove: one BE" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
	    'echo "want $((before + 1)) BEs, got $(ours | wc -l):"; ours | tail -3'
	check "upgrade --autoremove: says why" 'echo "$out" | grep -q "already made by this pkg run"' \
	    'echo "$out" | grep pkg-be'
	last=$(ours | tail -1); DS="$(zfs list -H -o name / | sed 's,/[^/]*$,,')/$last"
	check "upgrade --autoremove: status complete" '[ "$(zfs get -H -o value pkg-be:status "$DS")" = complete ]' \
	    'echo "got \"$(zfs get -H -o value pkg-be:status "$DS")\""'
	rm -f "$CONF"; pkg install -y "$DELPKG" >/dev/null
fi

# 29) PKG_RESTARTED (pkg restarted after upgrading itself): no new BE.
#     Simulated: the real restart needs an older pkg than the repository's.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
before=$(ours | wc -l)
out=$(env PKG_RESTARTED=1 pkg upgrade -fy "$PKGNAME" 2>&1)
check "PKG_RESTARTED: no BE" '[ "$(ours | wc -l)" -eq '"$before"' ]' 'echo "want $before, got $(ours | wc -l)"'
check "PKG_RESTARTED: says why" 'echo "$out" | grep -q "restarted after upgrading itself"' \
    'echo "$out" | grep pkg-be'

# 30) File conflict found after fetching: pkg applies the jobs a second
#     time (EPKG_CONFLICT, "One more solver iteration"). The pre hook runs
#     only when jobs are executed, so this must still give one BE.
#     A local repository: c1-1.0 owns f; c2-2.0 also ships f; c1-2.0
#     drops it. Served over HTTP so that pkg has to fetch (with file://
#     the conflict is solved before applying, which is not this path).
PY=$(command -v python3 || true)
if [ -n "$PY" ]; then
	CT=$(mktemp -d -t pkgbe-conflict)
	mkdir -p "$CT/repo" "$CT/inst" "$CT/repos"
	mkpkg() {	# NAME VERSION OUTDIR FILE...
		n=$1; v=$2; o=$3; shift 3
		r=$CT/root-$n-$v; d=/usr/local/share/pkgbe-itest-conflict
		mkdir -p "$r$d"; : > "$CT/plist-$n-$v"
		for f in "$@"; do
			echo "$n $v" > "$r$d/$f"; echo "$d/$f" >> "$CT/plist-$n-$v"
		done
		printf 'name: %s\nversion: "%s"\norigin: test/%s\ncomment: pkg-be test\ndesc: pkg-be test\nmaintainer: test@example.org\nwww: https://example.org\nprefix: /usr/local\nabi: "%s"\n' \
		    "$n" "$v" "$n" "$(pkg config ABI)" > "$CT/m-$n-$v"
		pkg create -M "$CT/m-$n-$v" -p "$CT/plist-$n-$v" -r "$r" -o "$o"
	}
	mkpkg pkgbe-itest-c1 1.0 "$CT/inst" f
	mkpkg pkgbe-itest-c2 1.0 "$CT/inst" g
	mkpkg pkgbe-itest-c1 2.0 "$CT/repo" h
	mkpkg pkgbe-itest-c2 2.0 "$CT/repo" f g
	pkg repo -q "$CT/repo"
	(cd "$CT/repo" && exec "$PY" -m http.server 18080 --bind 127.0.0.1) >/dev/null 2>&1 &
	HTTPD=$!; sleep 2
	printf 'pkgbe_itest: { url: "http://127.0.0.1:18080", signature_type: "none" }\n' > "$CT/repos/itest.conf"
	pkg add "$CT/inst/pkgbe-itest-c1-1.0.pkg" "$CT/inst/pkgbe-itest-c2-1.0.pkg" >/dev/null
	pkg -R "$CT/repos" update -q -f
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
	before=$(ours | wc -l)
	out=$(pkg -R "$CT/repos" upgrade -y -r pkgbe_itest 2>&1)
	check "conflict: pkg took the second-pass path" 'echo "$out" | grep -q "One more solver iteration"' \
	    'echo "pkg did not report EPKG_CONFLICT; this case was not exercised. Output:"; echo "$out" | tail -8'
	check "conflict: upgrade done" '[ "$(pkg query %v pkgbe-itest-c2)" = 2.0 ]' 'echo "c2 is $(pkg query %v pkgbe-itest-c2)"'
	check "conflict: one BE" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
	    'echo "want $((before + 1)), got $(ours | wc -l)"'
	check "conflict: hook ran once" '[ "$(echo "$out" | grep -c "creating boot environment")" -eq 1 ] && ! echo "$out" | grep -q "already made"' \
	    'echo "$out" | grep pkg-be'
	last=$(ours | tail -1); DS="$(zfs list -H -o name / | sed 's,/[^/]*$,,')/$last"
	check "conflict: status complete" '[ "$(zfs get -H -o value pkg-be:status "$DS")" = complete ]'
	kill "$HTTPD"; HTTPD=""
	pkg delete -qy -g 'pkgbe-itest-c[12]' >/dev/null
	rm -rf "$CT" /usr/local/share/pkgbe-itest-conflict; CT=""
else
	echo "skip - no python3 for a local HTTP repository, conflict case not tested"
fi


# 31) Invalid BE_PREFIX is refused when pkg loads the plugin, before
#     any snapshot. libbe alone accepted "bad/name" and leaked a snapshot.
for bp in "bad/name" "a b" "-x" "a@b" "$(printf 'a%.0s' $(seq 65))"; do
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$bp" > "$CONF"
	s0=$(nsnap); b0=$(bectl list -H | wc -l)
	out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
	check "BE_PREFIX \"$bp\": error shown" 'echo "$out" | grep -q "invalid BE_PREFIX"' \
	    'echo "want \"invalid BE_PREFIX\", output:"; echo "$out" | grep pkg-be'
	check "BE_PREFIX \"$bp\": no BE" '[ "$(bectl list -H | wc -l)" -eq '"$b0"' ]' \
	    'echo "BEs: want $b0, got $(bectl list -H | wc -l)"'
	check "BE_PREFIX \"$bp\": no snapshot left" '[ "$(nsnap)" -eq '"$s0"' ]' \
	    'echo "snapshots: want $s0, got $(nsnap):"; zfs list -H -t snapshot -o name -r "$ROOTDS"'
done

# 32) If the clone fails after the snapshot, the snapshot is removed.
#     Needs a test build (make PKGBE_TESTING=1): PKGBE_TEST_FAIL_CLONE
#     makes libbe's clone fail for real.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
SO=$(pkg config PKG_PLUGINS_DIR); SO=${SO%/}/pkg-be.so
TB=$(mktemp -d -t pkgbe-build)
cp -R "$SRC/" "$TB/src"
if (cd "$TB/src" && make clean >/dev/null 2>&1; find . -name '.depend*' -delete;
    make PKGBE_TESTING=1 >/dev/null 2>&1) && [ -f "$TB/src/pkg-be.so" ]; then
	SO_BAK=$(mktemp -t pkgbe-so); cp "$SO" "$SO_BAK"
	cat "$TB/src/pkg-be.so" > "$SO"
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
	s0=$(nsnap); before=$(ours | wc -l)
	out=$(env PKGBE_TEST_FAIL_CLONE=1 pkg upgrade -fy "$PKGNAME" 2>&1)
	check "clone failure: reported" 'echo "$out" | grep -q "could not create boot environment"' \
	    'echo "$out" | grep pkg-be'
	check "clone failure: snapshot removed" 'echo "$out" | grep -q "removed snapshot"' \
	    'echo "$out" | grep pkg-be'
	check "clone failure: no snapshot left" '[ "$(nsnap)" -eq '"$s0"' ]' \
	    'echo "snapshots: want $s0, got $(nsnap):"; zfs list -H -t snapshot -o name -r "$ROOTDS"'
	check "clone failure: no BE" '[ "$(ours | wc -l)" -eq '"$before"' ]'
	check "clone failure: pkg still succeeded" '[ "$(pkg query %n "$PKGNAME")" = "$PKGNAME" ]'
	# Same test build, no failure: normal path still makes a BE.
	out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
	check "test build, no failure: BE made" '[ "$(ours | wc -l)" -eq $((before + 1)) ]' \
	    'echo "$out" | grep pkg-be'
	cat "$SO_BAK" > "$SO"; rm -f "$SO_BAK"; SO_BAK=""
else
	echo "FAIL - could not build the PKGBE_TESTING plugin"; fail=1
fi
rm -rf "$TB"

# 33) Partial clone: libbe clones the BE root, then fails on a child
#      dataset and leaves the root clone behind. Real failure, no test
#      build: a child of the running BE whose name fits (219 bytes, and
#      its snapshot name) but not under a BE named with a 40-character
#      prefix (ZFS names are at most 255 bytes).
BEDS=$(zfs list -H -o name /)
LONGC=$(printf 'c%.0s' $(seq 200)); LONGP=$(printf 'p%.0s' $(seq 40))
CHILD=$BEDS/$LONGC
if zfs create -o canmount=off "$CHILD"; then
	printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$LONGP" > "$CONF"
	s0=$(nsnap); b0=$(bectl list -H | wc -l)
	out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
	check "partial clone: failure reported" 'echo "$out" | grep -q "could not create boot environment"' \
	    'echo "pkg-be did not fail; the case was not exercised:"; echo "$out" | grep pkg-be'
	check "partial clone: partial BE removed" 'echo "$out" | grep -q "removed the partial boot environment"' \
	    'echo "$out" | grep pkg-be'
	check "partial clone: no dataset left" '! zfs list -H -o name -d1 "$ROOTDS" | grep -q "/$LONGP"' \
	    'echo "left behind:"; zfs list -H -o name -r "$ROOTDS" | grep "$LONGP"'
	check "partial clone: snapshot removed" 'echo "$out" | grep -q "removed snapshot"' \
	    'echo "$out" | grep pkg-be'
	check "partial clone: no snapshot left" '[ "$(nsnap)" -eq '"$s0"' ]' \
	    'echo "snapshots: want $s0, got $(nsnap):"; zfs list -H -t snapshot -o name -r "$ROOTDS" | cut -c1-80'
	check "partial clone: BE count unchanged" '[ "$(bectl list -H | wc -l)" -eq '"$b0"' ]'
	for d in $(zfs list -H -o name -d1 "$ROOTDS" | grep "/$LONGP"); do zfs destroy -r "$d"; done
	zfs destroy -r "$CHILD"
else
	echo "FAIL - could not create $CHILD"; fail=1
fi

# 34) Pruning still removes the origin snapshot (the snapshot name is
#     libbe's own, as with be_create()).
printf 'BE_PREFIX = "%s";\nBE_KEEP = 1;\n' "$PREFIX" > "$CONF"
pkg upgrade -fy "$PKGNAME" >/dev/null
pkg upgrade -fy "$PKGNAME" >/dev/null
check "BE_KEEP=1: one BE" '[ "$(ours | wc -l)" -eq 1 ]' 'ours'
check "BE_KEEP=1: one origin snapshot for it" '[ "$(nsnap)" -eq $((NSNAP0 + 1)) ]' \
    'echo "want $((NSNAP0 + 1)) (before the test + the kept BE), got:"; zfs list -H -t snapshot -o name -r "$ROOTDS"'

# 35) A negative BE_KEEP_DAYS does not suggest a negative BE_KEEP_AGE.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 100;\nBE_KEEP_DAYS = -5;\n' "$PREFIX" > "$CONF"
out=$(pkg be list 2>&1)
check "BE_KEEP_DAYS=-5: removal warned" 'echo "$out" | grep -q "BE_KEEP_DAYS was removed"'
check "BE_KEEP_DAYS=-5: no \"-5d\" advice" '! echo "$out" | grep -q -- "-5d"' 'echo "$out" | head -2'

# 36) Rollback without a terminal refuses, with a reason; "n" on a
#     terminal says so. The next boot BE does not change.
printf 'BE_PREFIX = "%s";\nBE_KEEP = 0;\n' "$PREFIX" > "$CONF"
last=$(ours | tail -1)
nb=$(bectl list -H | awk '$2 ~ /R/ {print $1}')
rc=0; out=$(pkg be rollback "$last" < /dev/null 2>&1) || rc=$?
check "rollback, no terminal: exit 64" '[ $rc -eq 64 ]' 'echo "got $rc; output: $out"'
check "rollback, no terminal: says why" 'echo "$out" | grep -q "no terminal.*use -y"' 'echo "$out"'
check "rollback, no terminal: next boot unchanged" '[ "$(bectl list -H | awk "\$2 ~ /R/ {print \$1}")" = "$nb" ]'
out=$(echo n | script -q /dev/null pkg be rollback "$last" 2>&1 || true)
check "rollback, answer n: says not activated" 'echo "$out" | grep -q "not activated"' 'echo "$out"'
check "rollback, answer n: next boot unchanged" '[ "$(bectl list -H | awk "\$2 ~ /R/ {print \$1}")" = "$nb" ]'

# 37) Diff works on a BE that is already mounted, and leaves it mounted.
MNTBE=$last; m=$(mktemp -d -t pkgbe-mnt)
bectl mount "$MNTBE" "$m" >/dev/null
rc=0; out=$(pkg be diff "$MNTBE" 2>&1) || rc=$?
check "diff on mounted BE: exit 0" '[ $rc -eq 0 ]' 'echo "got $rc: $out"'
check "diff on mounted BE: still mounted" 'mount -p | awk "{print \$2}" | grep -qx "$m"'
bectl umount "$MNTBE"; MNTBE=""; rmdir "$m" 2>/dev/null || true

# 38) List shows the status column.
out=$(pkg be list)
check "list: STATUS header" 'echo "$out" | head -1 | grep -q STATUS' 'echo "$out" | head -1'
check "list: complete shown" 'echo "$out" | grep "^$last " | grep -q complete' 'echo "$out" | grep "^$last "'

# 39) Pkg's database on a dataset outside the BE -> warning; the BE is
#     still made. A copy of the database on its own dataset, via -o PKG_DBDIR.
DBDS=$ROOTDS; DBDS=${DBDS%%/*}/pkgbe-itest-db
zfs create -o mountpoint=/var/db/pkgbe-itest-db "$DBDS"
cp -Rp /var/db/pkg/. /var/db/pkgbe-itest-db/
before=$(ours | wc -l)
out=$(pkg -o PKG_DBDIR=/var/db/pkgbe-itest-db upgrade -fy "$PKGNAME" 2>&1)
check "dbdir outside BE: warned" 'echo "$out" | grep -q "/var/db/pkgbe-itest-db is on $DBDS, outside"' \
    'echo "$out" | grep pkg-be'
check "dbdir outside BE: BE still made" '[ "$(ours | wc -l)" -eq $((before + 1)) ]'
zfs destroy -r "$DBDS"; DBDS=""
out=$(pkg upgrade -fy "$PKGNAME" 2>&1)
check "default layout: no dataset warning" '! echo "$out" | grep -q "outside the boot environment"' \
    'echo "$out" | grep pkg-be'

[ $fail -eq 0 ] && echo "PASS" || echo "FAILED"
exit $fail
