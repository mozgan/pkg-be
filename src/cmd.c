/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 *
 * "pkg be" subcommand: list, show, diff, rollback, prune.
 */

#include <sys/types.h>

#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "pkg-be.h"
#include "be_util.h"

#define LIST_PKG_WIDTH	50

static void
usage(void)
{
	fprintf(stderr,
	    "usage: pkg be list [-av]\n"
	    "       pkg be show <be>\n"
	    "       pkg be diff <be> [<be2>]\n"
	    "       pkg be rollback [-ty] <be>\n"
	    "       pkg be prune [-n]\n");
}

static libbe_handle_t *
open_be(void)
{
	libbe_handle_t *be;

	if ((be = libbe_init(NULL)) == NULL)
		warnx("root is not on ZFS boot environments");
	else
		libbe_print_on_error(be, false);
	return (be);
}

static bool
need_root(void)
{
	if (geteuid() == 0)
		return (true);
	warnx("this command must be run as root");
	return (false);
}

/* Read creation time and pkg-be properties of every BE. */
static int
load_recs(libbe_handle_t *be, struct pkgbe_rec **recs, size_t *n)
{
	char *out = NULL;
	const char *root = be_root_path(be);
	char *argv[] = { __DECONST(char *, ZFS_CMD), __DECONST(char *, "get"),
	    __DECONST(char *, "-Hp"), __DECONST(char *, "-d"),
	    __DECONST(char *, "1"), __DECONST(char *, "-o"),
	    __DECONST(char *, "name,property,value"),
	    __DECONST(char *, "creation," PROP_NS "op," PROP_NS "count,"
	    PROP_NS "packages"), __DECONST(char *, root), NULL };
	int rc;

	if (pkgbe_run(argv, &out) != 0) {
		warnx("zfs get failed");
		free(out);
		return (-1);
	}
	rc = pkgbe_parse_props(out, root, recs, n);
	free(out);
	return (rc);
}

static const struct pkgbe_rec *
find_rec(const struct pkgbe_rec *r, size_t n, const char *name)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(r[i].name, name) == 0)
			return (&r[i]);
	return (NULL);
}

static void
fmt_time(long long t, char *buf, size_t len)
{
	time_t tt = (time_t)t;
	struct tm tm;

	if (t <= 0 || localtime_r(&tt, &tm) == NULL ||
	    strftime(buf, len, "%Y-%m-%d %H:%M", &tm) == 0)
		strlcpy(buf, "-", len);
}

static const char *
flags(libbe_handle_t *be, const char *name)
{
	bool a = strcmp(name, be_active_name(be)) == 0;
	bool nb = strcmp(name, be_nextboot_name(be)) == 0;

	return (a && nb ? "NR" : a ? "N" : nb ? "R" : "-");
}

static int
cmd_list(libbe_handle_t *be, struct pkgbe_conf *c, int argc, char **argv)
{
	struct pkgbe_rec *r;
	size_t n;
	char when[32];
	bool all = false, verbose = false;
	int ch;

	while ((ch = getopt(argc, argv, "av")) != -1) {
		switch (ch) {
		case 'a':
			all = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			usage();
			return (EX_USAGE);
		}
	}
	if (load_recs(be, &r, &n) != 0)
		return (EX_SOFTWARE);
	printf("%-28s %-6s %-16s %-10s %5s  %s\n", "BE", "ACTIVE", "CREATED",
	    "OP", "PKGS", "CHANGES");
	for (size_t i = 0; i < n; i++) {
		if (!all && !pkgbe_is_ours(r[i].name, c->prefix))
			continue;
		fmt_time(r[i].created, when, sizeof(when));
		printf("%-28s %-6s %-16s %-10s ", r[i].name,
		    flags(be, r[i].name), when, r[i].op ? r[i].op : "-");
		if (r[i].count >= 0)
			printf("%5d  ", r[i].count);
		else
			printf("%5s  ", "-");
		if (r[i].packages == NULL)
			printf("-\n");
		else if (verbose || strlen(r[i].packages) <= LIST_PKG_WIDTH)
			printf("%s\n", r[i].packages);
		else
			printf("%.*s...\n", LIST_PKG_WIDTH - 3, r[i].packages);
	}
	pkgbe_free_recs(r, n);
	return (EX_OK);
}

static int
cmd_show(libbe_handle_t *be, const char *name)
{
	struct pkgbe_rec *r;
	const struct pkgbe_rec *e;
	size_t n;
	char when[32], ds[BE_MAXPATHLEN];
	const char *p, *q;
	int rc = EX_OK;

	if (load_recs(be, &r, &n) != 0)
		return (EX_SOFTWARE);
	if ((e = find_rec(r, n, name)) == NULL) {
		warnx("no such boot environment: %s", name);
		rc = EX_DATAERR;
		goto out;
	}
	fmt_time(e->created, when, sizeof(when));
	if (be_root_concat(be, name, ds) != BE_ERR_SUCCESS)
		strlcpy(ds, "-", sizeof(ds));
	printf("Name:      %s\n", e->name);
	printf("Dataset:   %s\n", ds);
	printf("Active:    %s\n", flags(be, e->name));
	printf("Created:   %s\n", when);
	printf("Operation: %s\n", e->op ? e->op : "-");
	if (e->count >= 0)
		printf("Packages:  %d\n", e->count);
	else
		printf("Packages:  -\n");
	if (e->packages != NULL) {
		/* One change per line. */
		for (p = e->packages; *p != '\0';
		    p = q + (*q != '\0' ? 2 : 0)) {
			if ((q = strstr(p, ", ")) == NULL)
				q = p + strlen(p);
			printf("  %.*s\n", (int)(q - p), p);
		}
	}
out:
	pkgbe_free_recs(r, n);
	return (rc);
}

static char *
query_pkgs(const char *root)
{
	char *out = NULL;
	char *argv[] = { __DECONST(char *, PKG_CMD), __DECONST(char *, "-o"),
	    __DECONST(char *, "PKG_ENABLE_PLUGINS=false"),
	    __DECONST(char *, "-r"), __DECONST(char *, root),
	    __DECONST(char *, "query"), __DECONST(char *, "%n %v"), NULL };

	if (pkgbe_run(argv, &out) != 0) {
		free(out);
		return (NULL);
	}
	return (out != NULL ? out : strdup(""));
}

/*
 * Package list ("name version" lines) of a BE. The running BE is read
 * from "/"; any other BE is mounted temporarily. NULL on error.
 */
static char *
be_pkgs(libbe_handle_t *be, const char *name)
{
	char mnt[BE_MAXPATHLEN] = "";
	char *out;

	if (be_exists(be, name) != BE_ERR_SUCCESS) {
		warnx("no such boot environment: %s", name);
		return (NULL);
	}
	if (strcmp(name, be_active_name(be)) == 0)
		return (query_pkgs("/"));
	if (be_mount(be, name, NULL, 0, mnt) != BE_ERR_SUCCESS) {
		warnx("could not mount %s: %s", name,
		    libbe_error_description(be));
		return (NULL);
	}
	if ((out = query_pkgs(mnt)) == NULL)
		warnx("pkg query failed in %s", name);
	if (be_unmount(be, name, 0) != BE_ERR_SUCCESS)
		warnx("could not unmount %s", name);
	else
		(void)rmdir(mnt);
	return (out);
}

/* diff BE: BE -> running system. diff BE BE2: BE -> BE2. */
static int
cmd_diff(libbe_handle_t *be, const char *name, const char *name2)
{
	const char *target = name2 != NULL ? name2 : be_active_name(be);
	char *from = NULL, *to = NULL, **d = NULL;
	size_t n = 0;
	int rc = EX_SOFTWARE;

	if (!need_root())
		return (EX_NOPERM);
	if (strcmp(name, target) == 0) {
		warnx("nothing to compare: %s is %s", name,
		    name2 != NULL ? "given twice" :
		    "the running boot environment");
		return (EX_DATAERR);
	}
	if ((from = be_pkgs(be, name)) == NULL ||
	    (to = be_pkgs(be, target)) == NULL) {
		rc = EX_DATAERR;
		goto out;
	}
	if (pkgbe_pkgdiff(from, to, &d, &n) != 0) {
		warnx("out of memory");
		goto out;
	}
	if (name2 != NULL)
		printf("Changes from %s to %s:\n", name, name2);
	else
		printf("Changes from %s to the running system:\n", name);
	if (n == 0)
		printf("  (none)\n");
	for (size_t i = 0; i < n; i++)
		printf("  %s\n", d[i]);
	rc = EX_OK;
out:
	pkgbe_free_strv(d, n);
	free(from);
	free(to);
	return (rc);
}

static bool
confirm(const char *msg)
{
	char buf[16];

	printf("%s [y/N]: ", msg);
	fflush(stdout);
	if (fgets(buf, sizeof(buf), stdin) == NULL)
		return (false);
	return (buf[0] == 'y' || buf[0] == 'Y');
}

static int
cmd_rollback(libbe_handle_t *be, int argc, char **argv)
{
	bool temporary = false, yes = false;
	char msg[512];
	const char *name;
	int ch;

	while ((ch = getopt(argc, argv, "ty")) != -1) {
		switch (ch) {
		case 't':
			temporary = true;
			break;
		case 'y':
			yes = true;
			break;
		default:
			usage();
			return (EX_USAGE);
		}
	}
	if (argc - optind != 1) {
		usage();
		return (EX_USAGE);
	}
	name = argv[optind];
	if (!need_root())
		return (EX_NOPERM);
	if (be_exists(be, name) != BE_ERR_SUCCESS) {
		warnx("no such boot environment: %s", name);
		return (EX_DATAERR);
	}
	snprintf(msg, sizeof(msg), "Boot into %s %s?", name,
	    temporary ? "on the next reboot only" : "from now on");
	if (!yes && !confirm(msg))
		return (EX_OK);
	if (be_activate(be, name, temporary) != BE_ERR_SUCCESS) {
		warnx("could not activate %s: %s", name,
		    libbe_error_description(be));
		return (EX_SOFTWARE);
	}
	printf("%s activated%s. Reboot to use it.\n", name,
	    temporary ? " for the next boot" : "");
	printf("Note: datasets outside the boot environment "
	    "(e.g. /home) are not rolled back.\n");
	return (EX_OK);
}

static int
cmd_prune(libbe_handle_t *be, struct pkgbe_conf *c, int argc, char **argv)
{
	bool dry = false;
	size_t n;
	int ch;

	while ((ch = getopt(argc, argv, "n")) != -1) {
		switch (ch) {
		case 'n':
			dry = true;
			break;
		default:
			usage();
			return (EX_USAGE);
		}
	}
	if (!dry && !need_root())
		return (EX_NOPERM);
	if (c->bad != NULL) {
		fprintf(stderr, "invalid %s \"%s\" in %s.conf, not pruning\n",
		    c->bad, c->bad_val, PLUGIN_NAME);
		return (EX_CONFIG);
	}
	if (c->keep <= 0 && c->keep_age <= 0) {
		printf("BE_KEEP and BE_KEEP_AGE are 0: nothing to prune\n");
		return (EX_OK);
	}
	n = pkgbe_prune(be, c, dry);
	if (n == 0)
		printf("nothing to prune (BE_KEEP = %d, BE_KEEP_AGE = %jds, "
		    "BE_MIN_AGE = %jds)\n", c->keep, (intmax_t)c->keep_age,
		    (intmax_t)c->min_age);
	return (EX_OK);
}

int
pkgbe_cmd_exec(int argc, char **argv)
{
	struct pkgbe_conf c;
	libbe_handle_t *be;
	const char *sub;
	int rc;

	/* argv[0] is "be". */
	if (argc < 2) {
		usage();
		return (EX_USAGE);
	}
	sub = argv[1];
	argc--;
	argv++;
	optreset = 1;
	optind = 1;

	pkgbe_conf_read(&c);
	if ((be = open_be()) == NULL)
		return (EX_UNAVAILABLE);

	if (strcmp(sub, "list") == 0)
		rc = cmd_list(be, &c, argc, argv);
	else if (strcmp(sub, "show") == 0 && argc == 2)
		rc = cmd_show(be, argv[1]);
	else if (strcmp(sub, "diff") == 0 && (argc == 2 || argc == 3))
		rc = cmd_diff(be, argv[1], argc == 3 ? argv[2] : NULL);
	else if (strcmp(sub, "rollback") == 0)
		rc = cmd_rollback(be, argc, argv);
	else if (strcmp(sub, "prune") == 0)
		rc = cmd_prune(be, &c, argc, argv);
	else {
		usage();
		rc = EX_USAGE;
	}
	libbe_close(be);
	return (rc);
}
