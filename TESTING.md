# Testing pkg-be

Four layers: unit tests, the integration test, system tests (chroot, UFS,
pkgbase, reboot rollback) and the port test. Do them in this order.

All commands run on a FreeBSD 15.1 (or 14.x) system installed on ZFS.
Use a VM and take a snapshot before steps 3-5.

## 0. Prerequisites (once)

```
doas pkg install git atf FreeBSD-tests poudriere portlint
```
- `atf` (devel/atf) provides `libatf-c`. Base only ships a private copy
  (`libprivateatf-c`); the test Makefile falls back to it.
- On 15.1 the `FreeBSD-kyua` package does not contain Kyua's store schema
  (`/usr/share/kyua/store/*.sql`); it comes with `FreeBSD-tests`.

## 1. Unit tests (no root, no ZFS)

Scope: the pure functions in `src/be_util.c` (no libbe/libpkg calls).

| Function | What is tested |
|---|---|
| `pkgbe_make_name` | name, `-N` suffix, truncation, bad arguments |
| `pkgbe_is_ours` | only our BEs match; similar prefixes do not |
| `pkgbe_name_time` | time parsed back from a name |
| `pkgbe_select_prune` | BE_KEEP, BE_KEEP_AGE, both; newest kept; active/next-boot protected; `-10` after `-9`; BE_MIN_AGE protects young BEs, exact boundaries (1 h, 1 d), MIN_AGE over KEEP_AGE, MIN_AGE alone prunes nothing, future-dated BEs |
| `pkgbe_parse_duration` | s/m/h/d/w, no suffix, blanks, leading zeros, 2^31-1 limit; rejects empty, signs, fractions, compound, upper case, overflow; output unchanged on error |
| `pkgbe_repo_listed` | empty filter, lists with odd separators, no prefix match, case-sensitive, `local` = no repository |
| `pkgbe_parse_hooks` | BE_HOOKS words, case, `all`/`none`, errors |
| `pkgbe_join` | metadata list, `... (+N more)` truncation |
| `pkgbe_parse_props` | `zfs get -Hp` output for `pkg be list` |
| `pkgbe_pkgdiff` | added / removed / changed packages |
| `pkgbe_argv_rootdir`, `pkgbe_is_slash` | `-r DIR`, `-rDIR`, `--rootdir=DIR`, option arguments, subcommand options |

Steps:
1. `make test`
2. Expect `27/27 passed`.
3. Details: `cd tests && kyua report --verbose`
4. One case: `cd tests && ./be_util_test argv_rootdir`

Adding a test: write `ATF_TC_WITHOUT_HEAD` + `ATF_TC_BODY` in
`tests/be_util_test.c`, register it in `ATF_TP_ADD_TCS`.

## 2. Integration test (root, live ZFS)

`tests/integration.sh` runs real pkg commands. It writes a temporary
config (`BE_PREFIX=pkgbe-itest`), restores the old one on exit and
destroys its BEs. Packages used: `pkgconf` (reinstalled) and
`universal-ctags` (removed and reinstalled); override with `ITEST_PKG`
and `ITEST_DEL_PKG`.

Steps:
1. `make && doas make install`
2. Enable the plugin in `/usr/local/etc/pkg.conf`:
   `PKG_ENABLE_PLUGINS = true; PLUGINS = [ "pkg-be" ];`
3. `doas make itest`
4. Expect `PASS` at the end and no `FAIL` lines.
   - With `universal-ctags` (or `ITEST_DEL_PKG`) installed: 28 `ok` lines.
   - Without it: 24 `ok` lines; the 4 deinstall/diff checks are skipped
     (one `skip -` line is printed).
5. Exit status:
   - `doas sh tests/integration.sh`: 0 pass, 1 fail, 77 skipped
     (not root, no ZFS boot environments, or `ITEST_PKG` not
     installed). "pkg-be plugin not loaded" is a failure (1).
   - `doas make itest`: 0 pass; any other script status makes make(1)
     print `*** Error code N` and exit 1, so a skip shows as
     `Error code 77`.

What it checks: creation; metadata; BE_KEEP; foreign BEs kept;
BE_ENABLED=false; `-n`; BE_HOOKS install/upgrade/deinstall; `pkg be
list/show/diff/prune`; `pkg -r DIR` skipped; jail skipped; syslog;
BE_KEEP_DAYS; BE_MIN_FREE warning; `pkg be diff A B`; v0.2.0:
pkg.conf `SYSLOG=false`, syslog ident; BE_REPOSITORIES (match, near
misses, wrong case, `local`, delete uses the installed package's
repository); BE_KEEP_AGE; BE_MIN_AGE (and the same case without it);
invalid durations create but never prune, `pkg be prune` exits 78;
unquoted durations are dropped by pkg.

Failed checks print what was expected and what happened.

## 3. System tests (root, by hand)

### 3.1 chroot and UFS root
Build a minimal root on a UFS memory disk and on a ZFS dataset:
```
# helper: copy base + pkg + plugin into $1
mkroot() {
  mkdir -p $1
  tar -C / -cf - bin sbin lib libexec usr/bin usr/sbin usr/lib usr/libexec \
    usr/share/keys usr/share/misc etc var/db/pkg usr/local/sbin/pkg \
    usr/local/lib/libpkg.so.4 usr/local/lib/pkg usr/local/etc/pkg \
    usr/local/etc/pkg.conf | tar -C $1 -xpf -
  mkdir -p $1/dev $1/tmp $1/var/tmp $1/var/run $1/var/cache/pkg
  mount -t devfs devfs $1/dev
}
md=$(mdconfig -a -t swap -s 6g); newfs -U /dev/$md
mkdir -p /mnt/ufsroot; mount /dev/$md /mnt/ufsroot; mkroot /mnt/ufsroot
zfs create -o mountpoint=/zchroot zroot/zchroot; mkroot /zchroot
```
Run and compare `bectl list` before and after each:

| Command | Expected message | New BE? |
|---|---|---|
| `chroot /mnt/ufsroot pkg upgrade -fy pkgconf` | root is not on ZFS boot environments | no |
| `pkg -c /mnt/ufsroot upgrade -fy pkgconf` | root is not on ZFS boot environments | no |
| `pkg -c /zchroot upgrade -fy pkgconf` | root is not on ZFS boot environments | no |
| `pkg -r /zchroot upgrade -fy pkgconf` | pkg works on another root (-r) | no |
| `pkg --rootdir=/zchroot upgrade -fy pkgconf` | pkg works on another root (-r) | no |
| `pkg -r / upgrade -fy pkgconf` | creating boot environment | yes (same system) |

Note: with `-r DIR`, pkg loads plugins and pkg.conf from DIR. The plugin
must be the new version inside DIR too.

Cleanup:
```
umount /mnt/ufsroot/dev /mnt/ufsroot /zchroot/dev
mdconfig -d -u $md; zfs destroy -r zroot/zchroot
```

### 3.2 pkgbase (base system packages)
1. `pkg upgrade -fy FreeBSD-kyua` (or a real base update, e.g. 15.1-pN -> pN+1)
2. Expect: `creating boot environment before upgrade: ...`
3. `pkg be show <be>` lists `FreeBSD-kyua ...`.

### 3.3 Rollback with a real reboot
1. Snapshot the VM.
2. `pkg upgrade -fy pkgconf`; note the new BE name `R`.
3. `pkg install -y figlet` (a change after `R`).
4. `pkg be diff R` -> `+figlet-...`
5. `pkg be rollback -y R`; `bectl list` -> `R` has `R`.
6. `shutdown -r now`
7. After boot: `bectl list` -> `R` has `NR`; `mount | head -1` -> `.../R on /`;
   `pkg info -e figlet` fails (rolled back).
8. Retention protects the running BE: add `BE_KEEP = 1;`, run
   `pkg be prune -n`; `R` must not be listed. Restore the config.
9. Go back: `pkg be rollback -y default; shutdown -r now`; after boot
   `figlet` is there again. Clean up: `pkg delete -y figlet; bectl destroy -o R`.

### 3.4 Other manual checks

| Scenario | How | Expected |
|---|---|---|
| Same second | two upgrades back to back | second name ends in `-1` |
| Active BE protection | covered in 3.3 step 8 | |
| Invalid prefix | `BE_PREFIX = "a b/c";` | error, upgrade continues |
| Invalid hooks | `BE_HOOKS = "bogus";` | error, falls back to `upgrade` |
| Autoremove hook | `BE_HOOKS = "autoremove";` + `pkg autoremove` | 1 BE if there is work |
| pkg-static | `pkg-static upgrade -f tmux` | plugin not loaded, no BE (known limit) |
| Non-root CLI | `pkg be diff <be>` as a user | "must be run as root", exit 77 |
| Help | `pkg help be` | opens pkg-be(8) |

## 4. Port test (poudriere)

1. `doas poudriere jail -c -j 151amd64 -v 15.1-RELEASE -a amd64` (and `143amd64`, 14.3-RELEASE)
2. `doas poudriere ports -c -p default -m git+https -B main`
3. Copy `port/sysutils/pkg-be` into the ports tree; put the distfile in
   `/usr/ports/distfiles` (or let it fetch from GitHub after the tag exists).
4. `cd <ports>/sysutils/pkg-be && make makesum && portlint -AC`
5. `doas poudriere testport -j 151amd64 -p default sysutils/pkg-be`
6. `doas poudriere testport -j 143amd64 -p default sysutils/pkg-be`
7. Expect: build, stage, `check-plist`, package, install, deinstall all OK.

## 5. CI (GitHub Actions)

`.github/workflows/ci.yml` runs sections 1, 2 and 6 in FreeBSD VMs:

| Job | FreeBSD | What |
|---|---|---|
| build-test | 15.1, 14.5 | `make`, `make test` |
| lint | 15.1 | `make lint`, scan-build |
| integration | 15.1 ZFS root | `make install`, enable plugin, `tests/integration.sh` (0 or 77 = OK) |
| pkg-main | 15.1 | build pkg from `main`, then `make`, `make test` (may fail) |

`make test` uses kyua when its store schema exists, otherwise
`tests/run_atf.sh` runs each test case directly (same results). Force the
fallback locally with `make test KYUA_SCHEMA=/nonexistent`.

## 6. Static analysis
```
make lint                        # clang-tidy (uses .clang-tidy) + mandoc
scan-build19 --status-bugs make
```
Expect no findings.

`make lint` runs `clang-tidy19 src/*.c -- -Iinclude -Isrc -I/usr/local/include`.
Do not pass `-checks=...` on the command line: it is appended to the list
in `.clang-tidy` and turns the disabled checks back on. `.clang-tidy`
explains why four checks are disabled (they contradict style(9) or ask for
C11 Annex K functions that FreeBSD libc does not have).
The "N warnings generated" lines come from system headers and are
suppressed; they are not findings.
