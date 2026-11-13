# pkg-be

pkg(8) plugin: creates a ZFS boot environment (**be**) before `pkg upgrade` and keeps
only the newest `BE_KEEP` of them.

Status: v0.2.0.

## Build / install (FreeBSD)

    make
    doas make install
    doas cp /usr/local/etc/pkg/pkg-be.conf.sample /usr/local/etc/pkg/pkg-be.conf
    # /usr/local/etc/pkg.conf
    PKG_ENABLE_PLUGINS = true;
    PLUGINS = [ "pkg-be" ];

## Config (pkg-be.conf)

    BE_ENABLED = true;      # on/off
    BE_PREFIX  = "pkg-be";  # BE name prefix; only these are ever pruned
    BE_KEEP    = 5;         # keep newest N (0 = keep all)
    BE_HOOKS   = "upgrade"; # also: install, deinstall, autoremove, all, none
    BE_KEEP_AGE  = "0";     # also destroy BEs older than this, e.g. "30d" (0 = off)
    BE_MIN_AGE   = "0";     # never destroy BEs younger than this, e.g. "1d" (0 = off)
    BE_REPOSITORIES = "";   # only for packages from these repos, e.g. "FreeBSD-base"
                            #   "local" = pkg add; empty = any
    BE_MIN_FREE  = 10;      # warn below N% free in the pool (0 = off)
    BE_SYSLOG    = true;    # also log to syslog (off if pkg.conf SYSLOG = false)

Durations: `<n>[s|m|h|d|w]`, no suffix = seconds. `BE_KEEP_DAYS = N`
(v0.1) still works. An invalid duration disables pruning, never creation.

## pkg be

    pkg be list [-av]           # BEs created by pkg-be, with their changes
    pkg be show <be>
    pkg be diff <be> [<be2>]    # packages changed since <be>, or <be> -> <be2> (root)
    pkg be rollback [-ty] <be>  # boot into <be> on next reboot (root)
    pkg be prune [-n]           # apply BE_KEEP now

`pkg help be` opens the manual page.

## Metadata

Each BE gets ZFS user properties `pkg-be:op`, `pkg-be:packages`,
`pkg-be:count`, `pkg-be:version`:

    zfs get pkg-be:op,pkg-be:packages zroot/ROOT/pkg-be-20261110-123807

## Test

    make test           # unit tests (ATF/Kyua)
    doas make itest     # integration test on live ZFS

See TESTING.md.

## When pkg-be does nothing

A boot environment only makes sense when pkg changes the running system
and that system boots from ZFS. In these cases pkg-be creates nothing:

| Case                                                        | What pkg does                                                                                           | Why no BE                                                                                                                                                                                                                        |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pkg upgrade -n`                                            | Dry run: shows the plan, changes nothing.                                                               | Nothing to roll back. pkg does not call plugin hooks at all.                                                                                                                                                                     |
| `pkg -r DIR upgrade`                                        | Installs into another root directory (e.g. a mounted disk or image); the running system is not touched. | A BE of the running system would protect the wrong system. pkg-be finds `-r`/`--rootdir` on pkg's command line and skips. Note: pkg loads plugins and `pkg.conf` from `DIR`. `pkg -r /` is the running system and does get a BE. |
| `pkg -c DIR upgrade`                                        | `chroot(2)`s into `DIR` first, then runs as if `DIR` were `/`.                                          | Inside the chroot libbe cannot find the boot environments, so `libbe_init()` fails and pkg-be skips.                                                                                                                             |
| `pkg -j JAIL upgrade`, or pkg run inside a jail             | Attaches to the jail and changes the jail's packages.                                                   | A jail does not boot; BEs are for the host. pkg-be checks `security.jail.jailed` and skips.                                                                                                                                      |
| Root not on ZFS (e.g. UFS)                                  | Normal operation.                                                                                       | Boot environments need ZFS. Checked once when pkg loads the plugin: no hooks are registered, one message.                                                                                                                         |
| `pkg-static`                                                | Statically linked pkg, used for recovery.                                                               | It cannot load shared-object plugins; pkg disables plugins in it.                                                                                                                                                                |
| `BE_ENABLED = false`, or the operation is not in `BE_HOOKS` | Normal operation.                                                                                       | Turned off by configuration.                                                                                                                                                                                                     |
| No package from `BE_REPOSITORIES`                           | Normal operation.                                                                                       | Filtered out by configuration; pkg-be says so.                                                                                                                                                                                   |
| Nothing to do (e.g. system up to date)                      | No jobs.                                                                                                | pkg only calls the hook when there is work.                                                                                                                                                                                      |

## Port

`port/sysutils/pkg-be` is the FreeBSD port. `distinfo` is not kept in
the repository: take it from the GitHub release of the tag (GitHub's tag
tarball differs from a local `git archive`).

## Author

Mehmet Ozgan <mozgan@gmail.com>

## Related

pkg-autoupgrade also creates BEs, but only for its periodic auto-upgrade
command; pkg-be hooks every interactive `pkg upgrade`.
