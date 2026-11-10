/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 *
 * Pure helpers (no libbe/libpkg calls) so they can be unit tested.
 */
#ifndef PKG_BE_UTIL_H
#define PKG_BE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/*
 * "<prefix>-YYYYmmdd-HHMMSS" (+ "-<seq>" if seq > 0).
 * 0 on success, -1 on error or truncation.
 */
int	pkgbe_make_name(char *buf, size_t len, const char *prefix, time_t t,
	    int seq);

/* True if name was created by pkg-be with this prefix. */
bool	pkgbe_is_ours(const char *name, const char *prefix);

/* Creation time encoded in one of our names (local time). 0 or -1. */
int	pkgbe_name_time(const char *name, const char *prefix, time_t *t);

/* Retention policy. 0 disables a rule. */
struct pkgbe_policy {
	int	keep;		/* keep at most this many (newest) */
	int	keep_days;	/* destroy if older than this many days */
	time_t	now;		/* reference time for keep_days */
};

/*
 * Choose BEs to destroy. Only our names (pkgbe_is_ours) are considered.
 * A BE is chosen if it is not among the newest `keep`, or if it is older
 * than `keep_days`. The newest of ours is never chosen. `active` and
 * `nextboot` (may be NULL) are never chosen and are not replaced, so more
 * than `keep` may remain. Pointers to chosen names are stored in `out`
 * (capacity n). Returns how many were chosen. `names` is not modified.
 */
size_t	pkgbe_select_prune(const char *const *names, size_t n,
	    const char *prefix, const struct pkgbe_policy *pol,
	    const char *active, const char *nextboot, const char **out);

/*
 * Join items with ", " into buf (NUL-terminated). If they do not all fit,
 * end with " ... (+K more)". Returns the number of items written.
 * len must be large enough for the suffix (>= 32), else buf = "" and 0.
 */
size_t	pkgbe_join(char *buf, size_t len, const char *const *items, size_t n);

/* One boot environment as reported by "zfs get -Hp -o name,property,value". */
struct pkgbe_rec {
	char		*name;		/* BE name (last path component) */
	long long	 created;	/* epoch, 0 if unknown */
	char		*op;		/* pkg-be:op, NULL if unset */
	int		 count;		/* pkg-be:count, -1 if unset */
	char		*packages;	/* pkg-be:packages, NULL if unset */
};

/*
 * Parse "zfs get -Hp -o name,property,value -d 1 ... <root>" output.
 * Lines for <root> itself are skipped; "-" values count as unset.
 * Records are sorted oldest first. Returns 0, or -1 on allocation failure.
 */
int	pkgbe_parse_props(const char *text, const char *root,
	    struct pkgbe_rec **recs, size_t *n);
void	pkgbe_free_recs(struct pkgbe_rec *recs, size_t n);

/*
 * Compare two package lists ("name version" per line, any order).
 * Produces "+name-ver" (only in `to`), "-name-ver" (only in `from`) and
 * "name old->new" (version changed), sorted by name.
 * Returns 0, or -1 on allocation failure. Free with pkgbe_free_strv().
 */
int	pkgbe_pkgdiff(const char *from, const char *to, char ***out, size_t *n);
void	pkgbe_free_strv(char **v, size_t n);

/*
 * Given pkg(8)'s own argv, return the argument of -r/--rootdir among the
 * global options (those before the command), or NULL if there is none.
 * Follows pkg's option string "+dj:c:C:R:r:lNvo:46" and its long options.
 */
const char *pkgbe_argv_rootdir(int argc, char *const argv[]);

/* True for "/", "//", ... */
bool	pkgbe_is_slash(const char *path);

/* Operations that can trigger a boot environment (bit mask). */
#define PKGBE_HOOK_UPGRADE	0x1u
#define PKGBE_HOOK_INSTALL	0x2u
#define PKGBE_HOOK_DEINSTALL	0x4u
#define PKGBE_HOOK_AUTOREMOVE	0x8u

/*
 * Parse a BE_HOOKS value such as "upgrade, install" (comma and/or space
 * separated, case-insensitive; "all" = every hook, "" or "none" = no hook).
 * 0 on success; -1 on an unknown word, with *mask left unchanged.
 */
int	pkgbe_parse_hooks(const char *s, unsigned *mask);

#endif
