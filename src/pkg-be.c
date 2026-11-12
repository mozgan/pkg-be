/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 *
 * pkg-be: create a ZFS boot environment before pkg operations.
 * This file holds the pkg(8) plugin entry points and the hooks.
 */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "pkg-be.h"
#include "be_util.h"

#define DEFAULT_PREFIX	"pkg-be"
#define DEFAULT_KEEP	"5"
#define DEFAULT_HOOKS	"upgrade"
#define DEFAULT_KEEP_DAYS "0"
#define DEFAULT_MIN_FREE "10"
#define MAX_SEQ		99
/* ZFS user property values are limited to 8191 bytes. */
#define PKGLIST_MAX	4096

/* Entry points looked up by pkg(8) with dlsym(3). */
int pkg_plugin_init(struct pkg_plugin *);
int pkg_plugin_shutdown(struct pkg_plugin *);
int pkg_register_cmd_count(void);
int pkg_register_cmd(int, const char **, const char **, int (**)(int, char **));

extern char **environ;

struct pkg_plugin *pkgbe_self;

/* ---- configuration ---- */

static const pkg_object *
conf_find(const char *key)
{
	const pkg_object *conf, *o;
	pkg_iter it = NULL;

	if ((conf = pkg_plugin_conf(pkgbe_self)) == NULL)
		return (NULL);
	while ((o = pkg_object_iterate(conf, &it)) != NULL)
		if (strcmp(pkg_object_key(o), key) == 0)
			return (o);
	return (NULL);
}

void
pkgbe_conf_read(struct pkgbe_conf *c)
{
	const pkg_object *o;

	c->enabled = true;
	c->prefix = DEFAULT_PREFIX;
	c->keep = atoi(DEFAULT_KEEP);
	c->hooks = DEFAULT_HOOKS;
	c->keep_days = atoi(DEFAULT_KEEP_DAYS);
	c->min_free = atoi(DEFAULT_MIN_FREE);
	c->syslog = true;
	if ((o = conf_find("BE_KEEP_DAYS")) != NULL)
		c->keep_days = (int)pkg_object_int(o);
	if ((o = conf_find("BE_MIN_FREE")) != NULL)
		c->min_free = (int)pkg_object_int(o);
	if ((o = conf_find("BE_SYSLOG")) != NULL)
		c->syslog = pkg_object_bool(o);
	if ((o = conf_find("BE_ENABLED")) != NULL)
		c->enabled = pkg_object_bool(o);
	if ((o = conf_find("BE_PREFIX")) != NULL &&
	    pkg_object_string(o) != NULL)
		c->prefix = pkg_object_string(o);
	if ((o = conf_find("BE_KEEP")) != NULL)
		c->keep = (int)pkg_object_int(o);
	if ((o = conf_find("BE_HOOKS")) != NULL &&
	    pkg_object_string(o) != NULL)
		c->hooks = pkg_object_string(o);
}

/* ---- helpers ---- */

void
pkgbe_log(const struct pkgbe_conf *c, bool error, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (error)
		pkg_plugin_error(pkgbe_self, "%s", msg);
	else
		pkg_plugin_info(pkgbe_self, "%s", msg);
	if (c != NULL && c->syslog) {
		openlog(PLUGIN_NAME, LOG_PID, LOG_USER);
		syslog(error ? LOG_ERR : LOG_NOTICE, "%s", msg);
		closelog();
	}
}

/*
 * True if pkg was started as "pkg -r DIR ...": the operation then changes
 * DIR, not this system, so a BE of this system would be useless.
 * libpkg has no public getter for its root directory, so read our own
 * command line (kern.proc.args). "pkg -c DIR" chroots before plugins are
 * loaded; libbe_init() then fails and we skip anyway; "pkg -j" is caught
 * by in_jail().
 */
static bool
other_rootdir(void)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ARGS, (int)getpid() };
	char buf[8192], *argv[128];
	const char *root;
	size_t len = sizeof(buf) - 1, off = 0;
	int argc = 0;

	if (sysctl(mib, 4, buf, &len, NULL, 0) != 0)
		return (false);
	buf[len] = '\0';
	while (off < len && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
		argv[argc++] = buf + off;
		off += strlen(buf + off) + 1;
	}
	argv[argc] = NULL;
	root = pkgbe_argv_rootdir(argc, argv);
	return (root != NULL && !pkgbe_is_slash(root));
}

/* Percentage of free space in the pool that holds the BEs, -1 if unknown. */
static int
pool_free_pct(libbe_handle_t *be)
{
	char pool[256], *out = NULL, *slash;
	char *argv[] = { __DECONST(char *, "/sbin/zpool"),
	    __DECONST(char *, "list"), __DECONST(char *, "-Hp"),
	    __DECONST(char *, "-o"), __DECONST(char *, "capacity"), pool,
	    NULL };
	long cap;
	int rc = -1;

	strlcpy(pool, be_root_path(be), sizeof(pool));
	if ((slash = strchr(pool, '/')) != NULL)
		*slash = '\0';
	if (pkgbe_run(argv, &out) == 0 && out != NULL) {
		cap = strtol(out, NULL, 10);
		if (cap >= 0 && cap <= 100)
			rc = 100 - (int)cap;
	}
	free(out);
	return (rc);
}

int
pkgbe_run(char *const argv[], char **out)
{
	posix_spawn_file_actions_t fa;
	char *buf = NULL, *t;
	size_t len = 0, cap = 0;
	ssize_t r;
	pid_t pid;
	int fds[2] = { -1, -1 }, status, rc;

	if (out != NULL) {
		*out = NULL;
		if (pipe2(fds, O_CLOEXEC) != 0)
			return (-1);
	}
	posix_spawn_file_actions_init(&fa);
	if (out != NULL)
		posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
	rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (out != NULL)
		close(fds[1]);
	if (rc != 0) {
		if (out != NULL)
			close(fds[0]);
		return (-1);
	}
	if (out != NULL) {
		for (;;) {
			if (cap - len < 4096) {
				cap = cap != 0 ? cap * 2 : 8192;
				if ((t = realloc(buf, cap)) == NULL)
					break;
				buf = t;
			}
			r = read(fds[0], buf + len, cap - len - 1);
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				break;
			len += (size_t)r;
		}
		close(fds[0]);
		if (buf != NULL)
			buf[len] = '\0';
		*out = buf;
	}
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
		;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1);
}

static bool
in_jail(void)
{
	int jailed = 0;
	size_t len = sizeof(jailed);

	if (sysctlbyname("security.jail.jailed", &jailed, &len, NULL, 0) != 0)
		return (false);
	return (jailed != 0);
}

size_t
pkgbe_prune(libbe_handle_t *be, const struct pkgbe_conf *c, bool dry)
{
	struct pkgbe_policy pol = { .keep = c->keep, .keep_days = c->keep_days,
	    .now = time(NULL) };
	nvlist_t *props;
	nvpair_t *nvp = NULL;
	const char **names = NULL, **doomed = NULL;
	size_t n = 0, cap = 0, nd = 0;

	if (c->keep <= 0 && c->keep_days <= 0)
		return (0);
	if (be_prop_list_alloc(&props) != 0)
		return (0);
	if (be_get_bootenv_props(be, props) != 0)
		goto out;
	while ((nvp = nvlist_next_nvpair(props, nvp)) != NULL)
		cap++;
	names = calloc(cap + 1, sizeof(*names));
	doomed = calloc(cap + 1, sizeof(*doomed));
	if (names == NULL || doomed == NULL)
		goto out;
	while ((nvp = nvlist_next_nvpair(props, nvp)) != NULL && n < cap)
		names[n++] = nvpair_name(nvp);

	nd = pkgbe_select_prune(names, n, c->prefix, &pol,
	    be_active_name(be), be_nextboot_name(be), doomed);
	for (size_t i = 0; i < nd; i++) {
		if (dry) {
			printf("would destroy %s\n", doomed[i]);
			continue;
		}
		pkgbe_log(c, false, "destroying old boot environment: %s",
		    doomed[i]);
		if (be_destroy(be, doomed[i],
		    BE_DESTROY_AUTOORIGIN) != BE_ERR_SUCCESS)
			pkgbe_log(c, true, "could not destroy %s: %s",
			    doomed[i], libbe_error_description(be));
	}
out:
	free(names);
	free(doomed);
	be_prop_list_free(props);
	return (nd);
}

/* ---- metadata ---- */

/*
 * Describe each job as "+name-ver", "-name-ver" or "name old->new".
 * Returns a malloc'd array of malloc'd strings; *np is its length.
 */
static char **
job_list(struct pkg_jobs *jobs, size_t *np)
{
	struct pkg *new = NULL, *old = NULL;
	void *iter = NULL;
	char **v, *s;
	int type, cap;
	size_t n = 0;

	*np = 0;
	if (jobs == NULL || (cap = pkg_jobs_count(jobs)) <= 0)
		return (NULL);
	if ((v = calloc((size_t)cap, sizeof(*v))) == NULL)
		return (NULL);
	while (pkg_jobs_iter(jobs, &iter, &new, &old, &type)) {
		s = NULL;
		switch (type) {
		case PKG_SOLVED_INSTALL:
			pkg_asprintf(&s, "+%n-%v", new, new);
			break;
		case PKG_SOLVED_DELETE:
		case PKG_SOLVED_UPGRADE_REMOVE:
			pkg_asprintf(&s, "-%n-%v", new, new);
			break;
		case PKG_SOLVED_UPGRADE:
		case PKG_SOLVED_UPGRADE_INSTALL:
			if (old != NULL)
				pkg_asprintf(&s, "%n %v->%v", new, old, new);
			else
				pkg_asprintf(&s, "%n-%v", new, new);
			break;
		default:
			break;
		}
		/* Iterate to the end: pkg_jobs_iter frees its state then. */
		if (s != NULL && n < (size_t)cap)
			v[n++] = s;
		else
			free(s);
	}
	*np = n;
	return (v);
}

static void
tag_be(const struct pkgbe_conf *c, libbe_handle_t *be, const char *name,
    const char *op, struct pkg_jobs *jobs)
{
	char ds[BE_MAXPATHLEN], list[PKGLIST_MAX];
	char *p_op = NULL, *p_list = NULL, *p_cnt = NULL, *p_ver = NULL;
	char *argv[8];
	char **items;
	size_t n;

	if (be_root_concat(be, name, ds) != BE_ERR_SUCCESS)
		return;
	items = job_list(jobs, &n);
	pkgbe_join(list, sizeof(list), (const char *const *)items, n);

	if (asprintf(&p_op, PROP_NS "op=%s", op) > 0 &&
	    asprintf(&p_list, PROP_NS "packages=%s", list) > 0 &&
	    asprintf(&p_cnt, PROP_NS "count=%zu", n) > 0 &&
	    asprintf(&p_ver, PROP_NS "version=%s", PLUGIN_VERSION) > 0) {
		argv[0] = __DECONST(char *, ZFS_CMD);
		argv[1] = __DECONST(char *, "set");
		argv[2] = p_op;
		argv[3] = p_list;
		argv[4] = p_cnt;
		argv[5] = p_ver;
		argv[6] = ds;
		argv[7] = NULL;
		if (pkgbe_run(argv, NULL) != 0)
			pkgbe_log(c, true, "could not set properties on %s",
			    ds);
	}
	free(p_op);
	free(p_list);
	free(p_cnt);
	free(p_ver);
	pkgbe_free_strv(items, n);
}

/* ---- hooks ---- */

static int
make_be(const char *op, struct pkg_jobs *jobs)
{
	struct pkgbe_conf c;
	libbe_handle_t *be;
	char name[256];
	int seq, freep;
	time_t now;

	pkgbe_conf_read(&c);
	if (!c.enabled)
		return (EPKG_OK);
	if (other_rootdir()) {
		pkg_plugin_info(pkgbe_self, "pkg works on another root (-r), "
		    "skipping boot environment");
		return (EPKG_OK);
	}
	if (in_jail()) {
		pkg_plugin_info(pkgbe_self,
		    "running in a jail, skipping boot environment");
		return (EPKG_OK);
	}
	if ((be = libbe_init(NULL)) == NULL) {
		pkg_plugin_info(pkgbe_self,
		    "root is not on ZFS boot environments, skipping");
		return (EPKG_OK);
	}
	libbe_print_on_error(be, false);

	/* Find a free name: two runs in the same second get a -N suffix. */
	now = time(NULL);
	for (seq = 0; seq <= MAX_SEQ; seq++) {
		if (pkgbe_make_name(name, sizeof(name), c.prefix, now,
		    seq) != 0 || be_validate_name(be, name) != BE_ERR_SUCCESS) {
			pkgbe_log(&c, true, "invalid BE_PREFIX: %s", c.prefix);
			goto fail;
		}
		if (be_exists(be, name) != BE_ERR_SUCCESS)
			break;
	}
	if (seq > MAX_SEQ) {
		pkgbe_log(&c, true, "no free boot environment name");
		goto fail;
	}

	if (c.min_free > 0 && (freep = pool_free_pct(be)) >= 0 &&
	    freep < c.min_free)
		pkgbe_log(&c, true, "WARNING: only %d%% free in the pool; "
		    "old boot environments keep space in use (pkg be list)",
		    freep);

	pkgbe_log(&c, false, "creating boot environment before %s: %s",
	    op, name);
	if (be_create(be, name) != BE_ERR_SUCCESS) {
		/* pkg ignores our return value; the operation continues. */
		pkgbe_log(&c, true,
		    "WARNING: could not create boot environment %s: %s",
		    name, libbe_error_description(be));
		goto fail;
	}
	tag_be(&c, be, name, op, jobs);
	pkgbe_prune(be, &c, false);
	libbe_close(be);
	return (EPKG_OK);
fail:
	libbe_close(be);
	return (EPKG_FATAL);
}

static int
upgrade_cb(void *data, struct pkgdb *db __unused)
{
	return (make_be("upgrade", data));
}

static int
install_cb(void *data, struct pkgdb *db __unused)
{
	return (make_be("install", data));
}

static int
deinstall_cb(void *data, struct pkgdb *db __unused)
{
	return (make_be("deinstall", data));
}

static int
autoremove_cb(void *data, struct pkgdb *db __unused)
{
	return (make_be("autoremove", data));
}

static const struct {
	unsigned bit;
	pkg_plugin_hook_t hook;
	pkg_plugin_callback cb;
} hooks[] = {
	{ PKGBE_HOOK_UPGRADE, PKG_PLUGIN_HOOK_PRE_UPGRADE, upgrade_cb },
	{ PKGBE_HOOK_INSTALL, PKG_PLUGIN_HOOK_PRE_INSTALL, install_cb },
	{ PKGBE_HOOK_DEINSTALL, PKG_PLUGIN_HOOK_PRE_DEINSTALL, deinstall_cb },
	{ PKGBE_HOOK_AUTOREMOVE, PKG_PLUGIN_HOOK_PRE_AUTOREMOVE,
	    autoremove_cb },
};

/* ---- plugin entry points ---- */

int
pkg_plugin_init(struct pkg_plugin *p)
{
	struct pkgbe_conf c;
	unsigned mask;

	pkgbe_self = p;

	pkg_plugin_set(p, PKG_PLUGIN_NAME, PLUGIN_NAME);
	pkg_plugin_set(p, PKG_PLUGIN_DESC,
	    "Create a ZFS boot environment before pkg operations");
	pkg_plugin_set(p, PKG_PLUGIN_VERSION, PLUGIN_VERSION);

	pkg_plugin_conf_add(p, PKG_BOOL, "BE_ENABLED", "true");
	pkg_plugin_conf_add(p, PKG_STRING, "BE_PREFIX", DEFAULT_PREFIX);
	pkg_plugin_conf_add(p, PKG_INT, "BE_KEEP", DEFAULT_KEEP);
	pkg_plugin_conf_add(p, PKG_STRING, "BE_HOOKS", DEFAULT_HOOKS);
	pkg_plugin_conf_add(p, PKG_INT, "BE_KEEP_DAYS", DEFAULT_KEEP_DAYS);
	pkg_plugin_conf_add(p, PKG_INT, "BE_MIN_FREE", DEFAULT_MIN_FREE);
	pkg_plugin_conf_add(p, PKG_BOOL, "BE_SYSLOG", "true");
	pkg_plugin_parse(p);

	/*
	 * pkg 2.9.0 (fixed in 2.9.1) drops the values given to
	 * pkg_plugin_set(), and it finds the plugin's .conf file by that
	 * name, so pkg-be.conf is not read.
	 */
	if (pkg_plugin_get(p, PKG_PLUGIN_NAME) == NULL)
		pkg_plugin_error(p, "this pkg version does not keep plugin "
		    "names; %s.conf is ignored and defaults are used",
		    PLUGIN_NAME);

	pkgbe_conf_read(&c);
	if (pkgbe_parse_hooks(c.hooks, &mask) != 0) {
		pkg_plugin_error(p, "invalid BE_HOOKS \"%s\", using \"%s\"",
		    c.hooks, DEFAULT_HOOKS);
		(void)pkgbe_parse_hooks(DEFAULT_HOOKS, &mask);
	}

	for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		if ((mask & hooks[i].bit) == 0)
			continue;
		if (pkg_plugin_hook_register(p, hooks[i].hook,
		    hooks[i].cb) != EPKG_OK) {
			pkg_plugin_error(p, "failed to register hook");
			return (EPKG_FATAL);
		}
	}
	return (EPKG_OK);
}

int
pkg_plugin_shutdown(struct pkg_plugin *p __unused)
{
	return (EPKG_OK);
}

/* Adds the "pkg be" subcommand (see cmd.c). */
int
pkg_register_cmd_count(void)
{
	return (1);
}

int
pkg_register_cmd(int idx, const char **name, const char **desc,
    int (**exec)(int, char **))
{
	if (idx != 0)
		return (EPKG_FATAL);
	*name = "be";
	*desc = "List, compare and roll back pkg-be boot environments";
	*exec = pkgbe_cmd_exec;
	return (EPKG_OK);
}
