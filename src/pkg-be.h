/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 *
 * Declarations shared by the hook code (pkg-be.c) and the CLI (cmd.c).
 */
#ifndef PKG_BE_H
#define PKG_BE_H

#include <stdbool.h>

#include <pkg.h>
#include "be_compat.h"

#define PLUGIN_NAME	"pkg-be"
#define PLUGIN_VERSION	"0.1.0"
#define PROP_NS		"pkg-be:"
#define ZFS_CMD		"/sbin/zfs"
#ifndef PKG_CMD
#define PKG_CMD		"/usr/local/sbin/pkg"
#endif

struct pkgbe_conf {
	bool		 enabled;
	const char	*prefix;
	int		 keep;
	int		 keep_days;
	int		 min_free;	/* warn below this % free in the pool */
	bool		 syslog;
	const char	*hooks;
};

extern struct pkg_plugin *pkgbe_self;

void	pkgbe_conf_read(struct pkgbe_conf *);

/* posix_spawn argv (no shell); capture stdout into *out if non-NULL. */
int	pkgbe_run(char *const argv[], char **out);

/* Apply BE_KEEP / BE_KEEP_DAYS. dry: only print. Returns count. */
size_t	pkgbe_prune(libbe_handle_t *, const struct pkgbe_conf *, bool dry);

/* Message to pkg's output and, if enabled, syslog(3). */
void	pkgbe_log(const struct pkgbe_conf *, bool error, const char *fmt, ...)
	    __printflike(3, 4);

/* "pkg be ..." */
int	pkgbe_cmd_exec(int argc, char **argv);

#endif
