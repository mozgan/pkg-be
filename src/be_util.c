/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be_util.h"

int
pkgbe_make_name(char *buf, size_t len, const char *prefix, time_t t, int seq)
{
	struct tm tm;
	char ts[16];
	int n;

	if (buf == NULL || prefix == NULL || *prefix == '\0' || seq < 0)
		return (-1);
	if (localtime_r(&t, &tm) == NULL ||
	    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm) == 0)
		return (-1);
	if (seq == 0)
		n = snprintf(buf, len, "%s-%s", prefix, ts);
	else
		n = snprintf(buf, len, "%s-%s-%d", prefix, ts, seq);
	return (n < 0 || (size_t)n >= len ? -1 : 0);
}

static bool
digits(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!isdigit((unsigned char)s[i]))
			return (false);
	return (true);
}

bool
pkgbe_is_ours(const char *name, const char *prefix)
{
	size_t pl;
	const char *s;

	if (name == NULL || prefix == NULL || *prefix == '\0')
		return (false);
	pl = strlen(prefix);
	if (strncmp(name, prefix, pl) != 0 || name[pl] != '-')
		return (false);
	s = name + pl + 1;
	/* YYYYmmdd-HHMMSS */
	if (strlen(s) < 15 || !digits(s, 8) || s[8] != '-' || !digits(s + 9, 6))
		return (false);
	s += 15;
	if (*s == '\0')
		return (true);
	/* optional -<seq> */
	if (*s != '-' || s[1] == '\0')
		return (false);
	return (digits(s + 1, strlen(s + 1)));
}

/*
 * Order by timestamp, then numeric seq (so -10 sorts after -9).
 * A trailing "-N" shorter than 6 chars is a seq; "-HHMMSS" is not.
 */
static int
cmp_names(const void *a, const void *b)
{
	const char *x = *(const char *const *)a, *y = *(const char *const *)b;
	const char *dx = strrchr(x, '-'), *dy = strrchr(y, '-');
	int c;

	size_t lx = strlen(x), ly = strlen(y);
	long sx = 0, sy = 0;

	if (dx != NULL && (size_t)(dx - x) + 1 < lx && strlen(dx + 1) < 6) {
		sx = strtol(dx + 1, NULL, 10);
		lx = (size_t)(dx - x);
	}
	if (dy != NULL && (size_t)(dy - y) + 1 < ly && strlen(dy + 1) < 6) {
		sy = strtol(dy + 1, NULL, 10);
		ly = (size_t)(dy - y);
	}
	c = strncmp(x, y, lx < ly ? lx : ly);
	if (c == 0 && lx != ly)
		c = lx < ly ? -1 : 1;
	if (c != 0)
		return (c);
	return (sx < sy ? -1 : sx > sy);
}

static bool
same(const char *a, const char *b)
{
	return (a != NULL && b != NULL && strcmp(a, b) == 0);
}

int
pkgbe_name_time(const char *name, const char *prefix, time_t *t)
{
	struct tm tm;
	const char *s;

	if (!pkgbe_is_ours(name, prefix) || t == NULL)
		return (-1);
	s = name + strlen(prefix) + 1;
	memset(&tm, 0, sizeof(tm));
	if (sscanf(s, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon,
	    &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
		return (-1);
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_isdst = -1;
	if ((*t = mktime(&tm)) == (time_t)-1)
		return (-1);
	return (0);
}

size_t
pkgbe_select_prune(const char *const *names, size_t n, const char *prefix,
    const struct pkgbe_policy *pol, const char *active, const char *nextboot,
    const char **out)
{
	const char **ours;
	size_t cnt = 0, nout = 0, keep;
	time_t t, limit = 0, young = 0;
	bool by_count, by_age, known;

	if (pol == NULL || names == NULL || out == NULL || n == 0)
		return (0);
	if (pol->keep <= 0 && pol->keep_age <= 0)
		return (0);
	keep = pol->keep > 0 ? (size_t)pol->keep : 0;
	if (pol->keep_age > 0)
		limit = pol->now - pol->keep_age;
	if (pol->min_age > 0)
		young = pol->now - pol->min_age;
	if ((ours = calloc(n, sizeof(*ours))) == NULL)
		return (0);
	for (size_t i = 0; i < n; i++)
		if (pkgbe_is_ours(names[i], prefix))
			ours[cnt++] = names[i];
	qsort(ours, cnt, sizeof(*ours), cmp_names);

	/* Oldest first; never the newest (i == cnt - 1). */
	for (size_t i = 0; i + 1 < cnt; i++) {
		known = pkgbe_name_time(ours[i], prefix, &t) == 0;
		by_count = keep > 0 && i + keep < cnt;
		by_age = pol->keep_age > 0 && known && t < limit;
		if (!by_count && !by_age)
			continue;
		/* Too young (or age unknown): keep it. */
		if (pol->min_age > 0 && (!known || t > young))
			continue;
		if (same(ours[i], active) || same(ours[i], nextboot))
			continue;
		out[nout++] = ours[i];
	}
	free(ours);
	return (nout);
}

static const struct {
	const char *name;
	unsigned bit;
} hook_names[] = {
	{ "upgrade",	PKGBE_HOOK_UPGRADE },
	{ "install",	PKGBE_HOOK_INSTALL },
	{ "deinstall",	PKGBE_HOOK_DEINSTALL },
	{ "delete",	PKGBE_HOOK_DEINSTALL },
	{ "autoremove",	PKGBE_HOOK_AUTOREMOVE },
	{ "all",	PKGBE_HOOK_UPGRADE | PKGBE_HOOK_INSTALL |
			PKGBE_HOOK_DEINSTALL | PKGBE_HOOK_AUTOREMOVE },
	{ "none",	0 },
};

int
pkgbe_parse_hooks(const char *s, unsigned *mask)
{
	unsigned m = 0;
	size_t len, i;

	if (s == NULL || mask == NULL)
		return (-1);
	for (;;) {
		while (*s == ',' || isspace((unsigned char)*s))
			s++;
		if (*s == '\0')
			break;
		for (len = 0; s[len] != '\0' && s[len] != ',' &&
		    !isspace((unsigned char)s[len]); len++)
			;
		for (i = 0; i < sizeof(hook_names) / sizeof(hook_names[0]); i++)
			if (strlen(hook_names[i].name) == len &&
			    strncasecmp(s, hook_names[i].name, len) == 0)
				break;
		if (i == sizeof(hook_names) / sizeof(hook_names[0]))
			return (-1);
		m |= hook_names[i].bit;
		s += len;
	}
	*mask = m;
	return (0);
}

size_t
pkgbe_join(char *buf, size_t len, const char *const *items, size_t n)
{
	char more[32];
	size_t used = 0, i, need, mlen;

	if (buf == NULL || len < sizeof(more))
		return (0);
	buf[0] = '\0';
	for (i = 0; i < n; i++) {
		need = strlen(items[i]) + (i > 0 ? 2 : 0);
		/* Leave room for the "more" suffix unless this is the last. */
		mlen = (i + 1 < n) ? (size_t)snprintf(more, sizeof(more),
		    " ... (+%zu more)", n - i - 1) : 0;
		if (used + need + mlen >= len)
			break;
		if (i > 0) {
			memcpy(buf + used, ", ", 2);
			used += 2;
		}
		memcpy(buf + used, items[i], need - (i > 0 ? 2 : 0));
		used += need - (i > 0 ? 2 : 0);
		buf[used] = '\0';
	}
	if (i < n) {
		snprintf(more, sizeof(more), " ... (+%zu more)", n - i);
		if (i == 0)
			snprintf(buf, len, "(+%zu)", n);
		else
			strlcat(buf, more, len);
	}
	return (i);
}

/* ---- zfs get parser ---- */

static struct pkgbe_rec *
rec_get(struct pkgbe_rec **recs, size_t *n, size_t *cap, const char *name)
{
	struct pkgbe_rec *r;

	for (size_t i = 0; i < *n; i++)
		if (strcmp((*recs)[i].name, name) == 0)
			return (&(*recs)[i]);
	if (*n == *cap) {
		size_t nc = *cap != 0 ? *cap * 2 : 8;
		if ((r = realloc(*recs, nc * sizeof(*r))) == NULL)
			return (NULL);
		*recs = r;
		*cap = nc;
	}
	r = &(*recs)[*n];
	memset(r, 0, sizeof(*r));
	r->count = -1;
	if ((r->name = strdup(name)) == NULL)
		return (NULL);
	(*n)++;
	return (r);
}

static int
cmp_rec(const void *a, const void *b)
{
	const struct pkgbe_rec *x = a, *y = b;

	if (x->created != y->created)
		return (x->created < y->created ? -1 : 1);
	return (strcmp(x->name, y->name));
}

int
pkgbe_parse_props(const char *text, const char *root,
    struct pkgbe_rec **recs, size_t *n)
{
	struct pkgbe_rec *v = NULL, *r;
	size_t cnt = 0, cap = 0, rl;
	char *copy, *line, *save = NULL, *ds, *prop, *val;

	*recs = NULL;
	*n = 0;
	if (text == NULL || root == NULL)
		return (0);
	if ((copy = strdup(text)) == NULL)
		return (-1);
	rl = strlen(root);
	for (line = strtok_r(copy, "\n", &save); line != NULL;
	    line = strtok_r(NULL, "\n", &save)) {
		ds = line;
		if ((prop = strchr(ds, '\t')) == NULL)
			continue;
		*prop++ = '\0';
		if ((val = strchr(prop, '\t')) == NULL)
			continue;
		*val++ = '\0';
		/* Only direct children: "<root>/<name>". */
		if (strncmp(ds, root, rl) != 0 || ds[rl] != '/' ||
		    ds[rl + 1] == '\0' || strchr(ds + rl + 1, '/') != NULL)
			continue;
		if ((r = rec_get(&v, &cnt, &cap, ds + rl + 1)) == NULL)
			goto fail;
		if (strcmp(val, "-") == 0)
			continue;
		if (strcmp(prop, "creation") == 0)
			r->created = strtoll(val, NULL, 10);
		else if (strcmp(prop, "pkg-be:count") == 0)
			r->count = (int)strtol(val, NULL, 10);
		else if (strcmp(prop, "pkg-be:op") == 0) {
			free(r->op);
			if ((r->op = strdup(val)) == NULL)
				goto fail;
		} else if (strcmp(prop, "pkg-be:packages") == 0) {
			free(r->packages);
			if ((r->packages = strdup(val)) == NULL)
				goto fail;
		}
	}
	free(copy);
	if (cnt > 0)
		qsort(v, cnt, sizeof(*v), cmp_rec);
	*recs = v;
	*n = cnt;
	return (0);
fail:
	free(copy);
	pkgbe_free_recs(v, cnt);
	return (-1);
}

void
pkgbe_free_recs(struct pkgbe_rec *recs, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free(recs[i].name);
		free(recs[i].op);
		free(recs[i].packages);
	}
	free(recs);
}

/* ---- package list diff ---- */

struct nv {
	char *name;
	char *ver;
};

static int
cmp_nv(const void *a, const void *b)
{
	return (strcmp(((const struct nv *)a)->name,
	    ((const struct nv *)b)->name));
}

/* Split "name version\n..." in place into a sorted array. */
static struct nv *
split_list(char *buf, size_t *n)
{
	struct nv *v = NULL, *t;
	size_t cnt = 0, cap = 0;
	char *line, *save = NULL, *sp;

	for (line = strtok_r(buf, "\n", &save); line != NULL;
	    line = strtok_r(NULL, "\n", &save)) {
		if ((sp = strchr(line, ' ')) == NULL)
			continue;
		*sp = '\0';
		if (cnt == cap) {
			cap = cap != 0 ? cap * 2 : 64;
			if ((t = realloc(v, cap * sizeof(*v))) == NULL) {
				free(v);
				return (NULL);
			}
			v = t;
		}
		v[cnt].name = line;
		v[cnt].ver = sp + 1;
		cnt++;
	}
	if (cnt > 0)
		qsort(v, cnt, sizeof(*v), cmp_nv);
	*n = cnt;
	/* Empty list: return a non-NULL dummy so callers can tell from OOM. */
	return (v != NULL ? v : malloc(1));
}

static int
push(char ***out, size_t *n, size_t *cap, char *s)
{
	char **t;

	if (s == NULL)
		return (-1);
	if (*n == *cap) {
		*cap = *cap != 0 ? *cap * 2 : 16;
		if ((t = realloc(*out, *cap * sizeof(*t))) == NULL) {
			free(s);
			return (-1);
		}
		*out = t;
	}
	(*out)[(*n)++] = s;
	return (0);
}

int
pkgbe_pkgdiff(const char *from, const char *to, char ***out, size_t *n)
{
	char *fb = NULL, *tb = NULL, *s;
	struct nv *f = NULL, *t = NULL;
	size_t nf = 0, nt = 0, i = 0, j = 0, cap = 0;
	int rc = -1, c;

	*out = NULL;
	*n = 0;
	if ((fb = strdup(from ? from : "")) == NULL ||
	    (tb = strdup(to ? to : "")) == NULL ||
	    (f = split_list(fb, &nf)) == NULL ||
	    (t = split_list(tb, &nt)) == NULL)
		goto done;
	while (i < nf || j < nt) {
		if (i == nf)
			c = 1;
		else if (j == nt)
			c = -1;
		else
			c = strcmp(f[i].name, t[j].name);
		s = NULL;
		if (c < 0) {
			if (asprintf(&s, "-%s-%s", f[i].name, f[i].ver) < 0)
				s = NULL;
			i++;
		} else if (c > 0) {
			if (asprintf(&s, "+%s-%s", t[j].name, t[j].ver) < 0)
				s = NULL;
			j++;
		} else {
			if (strcmp(f[i].ver, t[j].ver) != 0 &&
			    asprintf(&s, "%s %s->%s", f[i].name, f[i].ver,
			    t[j].ver) < 0)
				goto done;
			i++;
			j++;
			if (s == NULL)
				continue;
		}
		if (push(out, n, &cap, s) != 0)
			goto done;
	}
	rc = 0;
done:
	if (rc != 0) {
		pkgbe_free_strv(*out, *n);
		*out = NULL;
		*n = 0;
	}
	free(f);
	free(t);
	free(fb);
	free(tb);
	return (rc);
}

void
pkgbe_free_strv(char **v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

/* ---- pkg command line ---- */

/* Short options of pkg(8) that take an argument. */
#define PKG_ARG_OPTS	"jcCRro"

#define NLONGOPTS	(sizeof(pkg_longopts) / sizeof(pkg_longopts[0]))

static const struct {
	const char *name;
	char opt;		/* short equivalent */
	bool arg;
} pkg_longopts[] = {
	{ "debug", 'd', false }, { "jail", 'j', true }, { "chroot", 'c', true },
	{ "config", 'C', true }, { "repo-conf-dir", 'R', true },
	{ "rootdir", 'r', true }, { "list", 'l', false },
	{ "version", 'v', false }, { "option", 'o', true },
	{ "only-ipv4", '4', false }, { "only-ipv6", '6', false },
};

const char *
pkgbe_argv_rootdir(int argc, char *const argv[])
{
	const char *a, *val, *eq, *found = NULL;
	size_t k, nl;
	int i;

	if (argv == NULL)
		return (NULL);
	for (i = 1; i < argc && argv[i] != NULL; i++) {
		a = argv[i];
		if (strcmp(a, "--") == 0 || a[0] != '-' || a[1] == '\0')
			break;		/* first non-option: the command */
		if (a[1] == '-') {
			a += 2;
			eq = strchr(a, '=');
			nl = eq != NULL ? (size_t)(eq - a) : strlen(a);
			for (k = 0; k < NLONGOPTS; k++)
				if (strlen(pkg_longopts[k].name) == nl &&
				    strncmp(a, pkg_longopts[k].name, nl) == 0)
					break;
			if (k == NLONGOPTS || !pkg_longopts[k].arg)
				continue;
			if (eq != NULL)
				val = eq + 1;
			else if (i + 1 < argc)
				val = argv[++i];
			else
				break;
			if (pkg_longopts[k].opt == 'r')
				found = val;
			continue;
		}
		/* Cluster of short options, e.g. "-d", "-dr/x", "-r", "/x". */
		for (a++; *a != '\0'; a++) {
			if (strchr(PKG_ARG_OPTS, *a) == NULL)
				continue;
			if (a[1] != '\0')
				val = a + 1;
			else if (i + 1 < argc)
				val = argv[++i];
			else
				return (found);
			if (*a == 'r')
				found = val;
			break;
		}
	}
	return (found);
}

bool
pkgbe_is_slash(const char *path)
{
	if (path == NULL || *path != '/')
		return (false);
	while (*path == '/')
		path++;
	return (*path == '\0');
}

/* ---- durations and repository filter ---- */

int
pkgbe_parse_duration(const char *s, time_t *out)
{
	/* At most ~68 years: fits any time_t, and now - age cannot overflow. */
	uintmax_t n = 0, mult = 1, max = (uintmax_t)INT32_MAX;
	const char *p;

	if (s == NULL || out == NULL)
		return (-1);
	while (isspace((unsigned char)*s))
		s++;
	if (!isdigit((unsigned char)*s))
		return (-1);	/* empty, sign or junk */
	for (p = s; isdigit((unsigned char)*p); p++) {
		if (n > (max - (uintmax_t)(*p - '0')) / 10)
			return (-1);
		n = n * 10 + (uintmax_t)(*p - '0');
	}
	switch (*p) {
	case 'w':
		mult = 7 * 86400;
		break;
	case 'd':
		mult = 86400;
		break;
	case 'h':
		mult = 3600;
		break;
	case 'm':
		mult = 60;
		break;
	case 's':
		break;
	default:
		p--;	/* no suffix */
		break;
	}
	p++;
	while (isspace((unsigned char)*p))
		p++;
	if (*p != '\0' || n > max / mult)
		return (-1);
	*out = (time_t)(n * mult);
	return (0);
}

bool
pkgbe_repo_listed(const char *filter, const char *repo)
{
	const char *s = filter;
	size_t len;
	bool empty = true;

	if (s == NULL)
		return (true);
	for (;;) {
		while (*s == ',' || isspace((unsigned char)*s))
			s++;
		if (*s == '\0')
			break;
		for (len = 0; s[len] != '\0' && s[len] != ',' &&
		    !isspace((unsigned char)s[len]); len++)
			;
		empty = false;
		if (len == 5 && strncmp(s, "local", 5) == 0) {
			if (repo == NULL || *repo == '\0')
				return (true);
		} else if (repo != NULL && strlen(repo) == len &&
		    strncmp(s, repo, len) == 0)
			return (true);
		s += len;
	}
	return (empty);
}
