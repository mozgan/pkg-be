/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 */

#include <atf-c.h>
#include <stdlib.h>
#include <string.h>

#include "be_util.h"

/* 2026-11-10 12:38:07 UTC */
#define T0	((time_t)1794314287)

static void
utc(void)
{
	setenv("TZ", "UTC", 1);
	tzset();
}

/* ---- pkgbe_make_name ---- */

ATF_TC_WITHOUT_HEAD(make_name_basic);
ATF_TC_BODY(make_name_basic, tc)
{
	char buf[64];

	utc();
	ATF_REQUIRE_EQ(0, pkgbe_make_name(buf, sizeof(buf), "pkg-be", T0, 0));
	ATF_CHECK_STREQ("pkg-be-20261110-123807", buf);
}

ATF_TC_WITHOUT_HEAD(make_name_seq);
ATF_TC_BODY(make_name_seq, tc)
{
	char buf[64];

	utc();
	ATF_REQUIRE_EQ(0, pkgbe_make_name(buf, sizeof(buf), "x", T0, 3));
	ATF_CHECK_STREQ("x-20261110-123807-3", buf);
}

ATF_TC_WITHOUT_HEAD(make_name_truncation);
ATF_TC_BODY(make_name_truncation, tc)
{
	char buf[10];

	ATF_CHECK_EQ(-1, pkgbe_make_name(buf, sizeof(buf), "pkg-be", T0, 0));
}

ATF_TC_WITHOUT_HEAD(make_name_bad_args);
ATF_TC_BODY(make_name_bad_args, tc)
{
	char buf[64];

	ATF_CHECK_EQ(-1, pkgbe_make_name(NULL, 64, "p", T0, 0));
	ATF_CHECK_EQ(-1, pkgbe_make_name(buf, sizeof(buf), NULL, T0, 0));
	ATF_CHECK_EQ(-1, pkgbe_make_name(buf, sizeof(buf), "", T0, 0));
	ATF_CHECK_EQ(-1, pkgbe_make_name(buf, sizeof(buf), "p", T0, -1));
}

/* ---- pkgbe_is_ours ---- */

ATF_TC_WITHOUT_HEAD(is_ours);
ATF_TC_BODY(is_ours, tc)
{
	ATF_CHECK(pkgbe_is_ours("pkg-be-20261110-123807", "pkg-be"));
	ATF_CHECK(pkgbe_is_ours("pkg-be-20261110-123807-2", "pkg-be"));
	ATF_CHECK(pkgbe_is_ours("pkg-be-20261110-123807-12", "pkg-be"));

	ATF_CHECK(!pkgbe_is_ours("default", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-mine", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-20261110-12380", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-20261110x123807", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-20261110-123807-", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-20261110-123807-a", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-bex-20261110-123807", "pkg-be"));
	/* Another prefix that starts with ours must not match. */
	ATF_CHECK(!pkgbe_is_ours("pkg-be-old-20261110-123807", "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours(NULL, "pkg-be"));
	ATF_CHECK(!pkgbe_is_ours("pkg-be-20261110-123807", ""));
}

/* ---- pkgbe_select_prune ---- */

static bool
contains(const char **v, size_t n, const char *s)
{
	for (size_t i = 0; i < n; i++)
		if (strcmp(v[i], s) == 0)
			return (true);
	return (false);
}

ATF_TC_WITHOUT_HEAD(prune_keeps_newest);
ATF_TC_BODY(prune_keeps_newest, tc)
{
	/* Deliberately unsorted. */
	const char *names[] = {
		"pkg-be-20261103-100000",
		"default",
		"pkg-be-20261101-100000",
		"pkg-be-20261105-100000",
		"pkg-be-20261102-100000",
		"manual-backup",
	};
	const char *out[6];
	size_t n;

	n = pkgbe_select_prune(names, 6, "pkg-be",
	    &(struct pkgbe_policy){ .keep = 2 }, "default", "default", out);
	ATF_REQUIRE_EQ(2, n);
	ATF_CHECK(contains(out, n, "pkg-be-20261101-100000"));
	ATF_CHECK(contains(out, n, "pkg-be-20261102-100000"));
}

ATF_TC_WITHOUT_HEAD(prune_never_foreign);
ATF_TC_BODY(prune_never_foreign, tc)
{
	const char *names[] = { "default", "manual-backup", "pkg-be-x" };
	const char *out[3];

	ATF_CHECK_EQ(0, pkgbe_select_prune(names, 3, "pkg-be",
	    &(struct pkgbe_policy){ .keep = 1, .keep_days = 1, .now = T0 },
	    NULL, NULL, out));
}

ATF_TC_WITHOUT_HEAD(prune_protects_active_nextboot);
ATF_TC_BODY(prune_protects_active_nextboot, tc)
{
	const char *names[] = {
		"pkg-be-20261101-100000",	/* active (booted from) */
		"pkg-be-20261102-100000",	/* nextboot */
		"pkg-be-20261103-100000",
		"pkg-be-20261104-100000",
	};
	const char *out[4];
	size_t n;

	n = pkgbe_select_prune(names, 4, "pkg-be",
	    &(struct pkgbe_policy){ .keep = 1 },
	    "pkg-be-20261101-100000", "pkg-be-20261102-100000", out);
	ATF_REQUIRE_EQ(1, n);
	ATF_CHECK_STREQ("pkg-be-20261103-100000", out[0]);
}

ATF_TC_WITHOUT_HEAD(prune_unlimited);
ATF_TC_BODY(prune_unlimited, tc)
{
	const char *names[] = {
		"pkg-be-20261101-100000", "pkg-be-20261102-100000",
	};
	const char *out[2];

	struct pkgbe_policy p0 = { 0 }, pneg = { .keep = -1, .keep_days = -1 },
	    p5 = { .keep = 5 };

	ATF_CHECK_EQ(0, pkgbe_select_prune(names, 2, "pkg-be", &p0,
	    NULL, NULL, out));
	ATF_CHECK_EQ(0, pkgbe_select_prune(names, 2, "pkg-be", &pneg,
	    NULL, NULL, out));
	ATF_CHECK_EQ(0, pkgbe_select_prune(names, 2, "pkg-be", &p5,
	    NULL, NULL, out));
	ATF_CHECK_EQ(0, pkgbe_select_prune(names, 2, "pkg-be", NULL,
	    NULL, NULL, out));
}

ATF_TC_WITHOUT_HEAD(prune_seq_order);
ATF_TC_BODY(prune_seq_order, tc)
{
	/* Same second: -10 is newer than -9 and than the unsuffixed one. */
	const char *names[] = {
		"pkg-be-20261110-123807-10",
		"pkg-be-20261110-123807",
		"pkg-be-20261110-123807-9",
	};
	const char *out[3];
	size_t n;

	n = pkgbe_select_prune(names, 3, "pkg-be",
	    &(struct pkgbe_policy){ .keep = 1 }, NULL, NULL, out);
	ATF_REQUIRE_EQ(2, n);
	ATF_CHECK(contains(out, n, "pkg-be-20261110-123807"));
	ATF_CHECK(contains(out, n, "pkg-be-20261110-123807-9"));
}

ATF_TC_WITHOUT_HEAD(prune_keep_days);
ATF_TC_BODY(prune_keep_days, tc)
{
	const char *names[] = {
		"pkg-be-20261020-120000",	/* 21 days before T0: old */
		"pkg-be-20261101-120000",	/* 9 days: old */
		"pkg-be-20261105-120000",	/* 5 days: kept */
		"pkg-be-20261110-120000",	/* newest */
	};
	const char *out[4];
	struct pkgbe_policy p = { .keep_days = 7, .now = T0 };
	size_t n;

	utc();
	n = pkgbe_select_prune(names, 4, "pkg-be", &p, NULL, NULL, out);
	ATF_REQUIRE_EQ(2, n);
	ATF_CHECK(contains(out, n, "pkg-be-20261020-120000"));
	ATF_CHECK(contains(out, n, "pkg-be-20261101-120000"));

	/* Both rules: count removes the 5-day one as well. */
	p.keep = 1;
	n = pkgbe_select_prune(names, 4, "pkg-be", &p, NULL, NULL, out);
	ATF_CHECK_EQ(3, n);
}

ATF_TC_WITHOUT_HEAD(prune_keep_days_never_newest);
ATF_TC_BODY(prune_keep_days_never_newest, tc)
{
	/* Even if every BE is old, the newest one stays. */
	const char *names[] = {
		"pkg-be-20250101-000000", "pkg-be-20250102-000000",
	};
	const char *out[2];
	struct pkgbe_policy p = { .keep_days = 1, .now = T0 };

	utc();
	ATF_REQUIRE_EQ(1, pkgbe_select_prune(names, 2, "pkg-be", &p, NULL,
	    NULL, out));
	ATF_CHECK_STREQ("pkg-be-20250101-000000", out[0]);
}

ATF_TC_WITHOUT_HEAD(name_time);
ATF_TC_BODY(name_time, tc)
{
	time_t t;

	utc();
	ATF_REQUIRE_EQ(0,
	    pkgbe_name_time("pkg-be-20261110-123807", "pkg-be", &t));
	ATF_CHECK_EQ(T0, t);
	ATF_REQUIRE_EQ(0,
	    pkgbe_name_time("pkg-be-20261110-123807-3", "pkg-be", &t));
	ATF_CHECK_EQ(T0, t);
	ATF_CHECK_EQ(-1, pkgbe_name_time("default", "pkg-be", &t));
	ATF_CHECK_EQ(-1, pkgbe_name_time("pkg-be-20261110-123807", "x", &t));
}

/* ---- pkgbe_join ---- */

ATF_TC_WITHOUT_HEAD(join_all_fit);
ATF_TC_BODY(join_all_fit, tc)
{
	const char *v[] = { "tmux 3.5->3.5a", "+vim-9.1", "-nano-8.0" };
	char buf[128];

	ATF_CHECK_EQ(3, pkgbe_join(buf, sizeof(buf), v, 3));
	ATF_CHECK_STREQ("tmux 3.5->3.5a, +vim-9.1, -nano-8.0", buf);
}

ATF_TC_WITHOUT_HEAD(join_empty);
ATF_TC_BODY(join_empty, tc)
{
	char buf[64] = "junk";

	ATF_CHECK_EQ(0, pkgbe_join(buf, sizeof(buf), NULL, 0));
	ATF_CHECK_STREQ("", buf);
}

ATF_TC_WITHOUT_HEAD(join_truncates);
ATF_TC_BODY(join_truncates, tc)
{
	const char *v[] = { "aaaaaaaaaa", "bbbbbbbbbb", "cccccccccc",
	    "dddddddddd" };
	char buf[40];
	size_t k;

	k = pkgbe_join(buf, sizeof(buf), v, 4);
	ATF_CHECK_EQ(2, k);
	ATF_CHECK_STREQ("aaaaaaaaaa, bbbbbbbbbb ... (+2 more)", buf);
	ATF_CHECK(strlen(buf) < sizeof(buf));
}

ATF_TC_WITHOUT_HEAD(join_first_too_long);
ATF_TC_BODY(join_first_too_long, tc)
{
	const char *v[] = {
	    "a-very-long-package-name-that-does-not-fit-at-all", "b" };
	char buf[32];

	ATF_CHECK_EQ(0, pkgbe_join(buf, sizeof(buf), v, 2));
	ATF_CHECK_STREQ("(+2)", buf);
}

ATF_TC_WITHOUT_HEAD(join_small_buffer);
ATF_TC_BODY(join_small_buffer, tc)
{
	const char *v[] = { "a" };
	char buf[8];

	ATF_CHECK_EQ(0, pkgbe_join(buf, sizeof(buf), v, 1));
}

/* ---- pkgbe_parse_props ---- */

ATF_TC_WITHOUT_HEAD(props_parse);
ATF_TC_BODY(props_parse, tc)
{
	const char *text =
	    "zroot/ROOT\tcreation\t1700000000\n"
	    "zroot/ROOT/default\tcreation\t1790000000\n"
	    "zroot/ROOT/default\tpkg-be:op\t-\n"
	    "zroot/ROOT/default\tpkg-be:count\t-\n"
	    "zroot/ROOT/pkg-be-20261110-123807\tcreation\t1794314287\n"
	    "zroot/ROOT/pkg-be-20261110-123807\tpkg-be:op\tupgrade\n"
	    "zroot/ROOT/pkg-be-20261110-123807\tpkg-be:count\t2\n"
	    "zroot/ROOT/pkg-be-20261110-123807\tpkg-be:packages\t"
	    "tmux 3.5->3.5a, +vim-9.1\n"
	    "zroot/ROOT/old\tcreation\t1600000000\n"
	    "zroot/ROOT/default/child\tcreation\t1\n"
	    "zroot/other/x\tcreation\t1\n"
	    "garbage line\n";
	struct pkgbe_rec *r;
	size_t n;

	ATF_REQUIRE_EQ(0, pkgbe_parse_props(text, "zroot/ROOT", &r, &n));
	ATF_REQUIRE_EQ(3, n);
	/* Oldest first. */
	ATF_CHECK_STREQ("old", r[0].name);
	ATF_CHECK_STREQ("default", r[1].name);
	ATF_CHECK(r[1].op == NULL);
	ATF_CHECK_EQ(-1, r[1].count);
	ATF_CHECK_STREQ("pkg-be-20261110-123807", r[2].name);
	ATF_CHECK_EQ(1794314287LL, r[2].created);
	ATF_CHECK_STREQ("upgrade", r[2].op);
	ATF_CHECK_EQ(2, r[2].count);
	ATF_CHECK_STREQ("tmux 3.5->3.5a, +vim-9.1", r[2].packages);
	pkgbe_free_recs(r, n);
}

ATF_TC_WITHOUT_HEAD(props_parse_empty);
ATF_TC_BODY(props_parse_empty, tc)
{
	struct pkgbe_rec *r;
	size_t n;

	ATF_REQUIRE_EQ(0, pkgbe_parse_props("", "zroot/ROOT", &r, &n));
	ATF_CHECK_EQ(0, n);
	ATF_REQUIRE_EQ(0, pkgbe_parse_props(NULL, "zroot/ROOT", &r, &n));
	ATF_CHECK_EQ(0, n);
	/* Root prefix must match a full path component. */
	ATF_REQUIRE_EQ(0, pkgbe_parse_props("zroot/ROOTX/a\tcreation\t1\n",
	    "zroot/ROOT", &r, &n));
	ATF_CHECK_EQ(0, n);
}

/* ---- pkgbe_pkgdiff ---- */

ATF_TC_WITHOUT_HEAD(pkgdiff_basic);
ATF_TC_BODY(pkgdiff_basic, tc)
{
	const char *from = "tmux 3.5\nnano 8.0\nvim 9.0\ncurl 8.1\n";
	const char *to = "vim 9.1\ncurl 8.1\ntmux 3.5\ngit 2.47\n";
	char **d;
	size_t n;

	ATF_REQUIRE_EQ(0, pkgbe_pkgdiff(from, to, &d, &n));
	ATF_REQUIRE_EQ(3, n);
	/* Sorted by name; unchanged curl and tmux omitted. */
	ATF_CHECK_STREQ("+git-2.47", d[0]);
	ATF_CHECK_STREQ("-nano-8.0", d[1]);
	ATF_CHECK_STREQ("vim 9.0->9.1", d[2]);
	pkgbe_free_strv(d, n);
}

ATF_TC_WITHOUT_HEAD(pkgdiff_same_and_empty);
ATF_TC_BODY(pkgdiff_same_and_empty, tc)
{
	char **d;
	size_t n;

	ATF_REQUIRE_EQ(0, pkgbe_pkgdiff("a 1\nb 2\n", "b 2\na 1\n", &d, &n));
	ATF_CHECK_EQ(0, n);
	pkgbe_free_strv(d, n);

	ATF_REQUIRE_EQ(0, pkgbe_pkgdiff("", "a 1\n", &d, &n));
	ATF_REQUIRE_EQ(1, n);
	ATF_CHECK_STREQ("+a-1", d[0]);
	pkgbe_free_strv(d, n);

	ATF_REQUIRE_EQ(0, pkgbe_pkgdiff("a 1\n", NULL, &d, &n));
	ATF_REQUIRE_EQ(1, n);
	ATF_CHECK_STREQ("-a-1", d[0]);
	pkgbe_free_strv(d, n);
}

ATF_TC_WITHOUT_HEAD(pkgdiff_versions_with_spaces_and_commas);
ATF_TC_BODY(pkgdiff_versions_with_spaces_and_commas, tc)
{
	char **d;
	size_t n;

	/* pkg versions may contain "," and "_" (e.g. 3.0.7,1, 1.2_3). */
	ATF_REQUIRE_EQ(0, pkgbe_pkgdiff("pkgconf 3.0.7,1\nx 1.2_3\n",
	    "pkgconf 3.0.8,1\nx 1.2_3\n", &d, &n));
	ATF_REQUIRE_EQ(1, n);
	ATF_CHECK_STREQ("pkgconf 3.0.7,1->3.0.8,1", d[0]);
	pkgbe_free_strv(d, n);
}

/* ---- pkgbe_argv_rootdir ---- */

static const char *
rd(size_t n, const char *const *v)
{
	return (pkgbe_argv_rootdir((int)n, __DECONST(char *const *, v)));
}

#define RD(...)	rd(sizeof((const char *const[]){ __VA_ARGS__ }) / \
	sizeof(const char *), (const char *const[]){ __VA_ARGS__ })

ATF_TC_WITHOUT_HEAD(argv_rootdir);
ATF_TC_BODY(argv_rootdir, tc)
{
	ATF_CHECK(RD("pkg", "upgrade", "-f", "tmux") == NULL);
	ATF_CHECK_STREQ("/mnt", RD("pkg", "-r", "/mnt", "upgrade"));
	ATF_CHECK_STREQ("/mnt", RD("pkg", "-r/mnt", "upgrade"));
	ATF_CHECK_STREQ("/mnt", RD("pkg", "-dr", "/mnt", "upgrade"));
	ATF_CHECK_STREQ("/mnt", RD("pkg", "-d", "-C", "/x.conf", "-r", "/mnt",
	    "install", "vim"));
	ATF_CHECK_STREQ("/mnt", RD("pkg", "--rootdir", "/mnt", "upgrade"));
	ATF_CHECK_STREQ("/mnt", RD("pkg", "--rootdir=/mnt", "upgrade"));
	/* Option arguments are not mistaken for -r. */
	ATF_CHECK(RD("pkg", "-o", "-r", "upgrade") == NULL);
	ATF_CHECK(RD("pkg", "-C", "-r", "upgrade") == NULL);
	ATF_CHECK(RD("pkg", "-c", "/chroot", "upgrade") == NULL);
	/* After the command, -r belongs to the subcommand. */
	ATF_CHECK(RD("pkg", "upgrade", "-r", "myrepo") == NULL);
	ATF_CHECK(RD("pkg", "--", "-r", "/mnt") == NULL);
	/* Missing argument. */
	ATF_CHECK(RD("pkg", "-r") == NULL);
	ATF_CHECK(pkgbe_argv_rootdir(0, NULL) == NULL);
}

ATF_TC_WITHOUT_HEAD(is_slash);
ATF_TC_BODY(is_slash, tc)
{
	ATF_CHECK(pkgbe_is_slash("/"));
	ATF_CHECK(pkgbe_is_slash("///"));
	ATF_CHECK(!pkgbe_is_slash("/mnt"));
	ATF_CHECK(!pkgbe_is_slash("mnt"));
	ATF_CHECK(!pkgbe_is_slash(""));
	ATF_CHECK(!pkgbe_is_slash(NULL));
}

/* ---- pkgbe_parse_hooks ---- */

ATF_TC_WITHOUT_HEAD(hooks_parse);
ATF_TC_BODY(hooks_parse, tc)
{
	unsigned m;

	ATF_REQUIRE_EQ(0, pkgbe_parse_hooks("upgrade", &m));
	ATF_CHECK_EQ(PKGBE_HOOK_UPGRADE, m);
	ATF_REQUIRE_EQ(0, pkgbe_parse_hooks(" upgrade, Install ,delete", &m));
	ATF_CHECK_EQ(PKGBE_HOOK_UPGRADE | PKGBE_HOOK_INSTALL |
	    PKGBE_HOOK_DEINSTALL, m);
	ATF_REQUIRE_EQ(0, pkgbe_parse_hooks("deinstall autoremove", &m));
	ATF_CHECK_EQ(PKGBE_HOOK_DEINSTALL | PKGBE_HOOK_AUTOREMOVE, m);
	ATF_REQUIRE_EQ(0, pkgbe_parse_hooks("all", &m));
	ATF_CHECK_EQ(0xfu, m);
	ATF_REQUIRE_EQ(0, pkgbe_parse_hooks("none", &m));
	ATF_CHECK_EQ(0, m);
	ATF_REQUIRE_EQ(0, pkgbe_parse_hooks("", &m));
	ATF_CHECK_EQ(0, m);
}

ATF_TC_WITHOUT_HEAD(hooks_parse_invalid);
ATF_TC_BODY(hooks_parse_invalid, tc)
{
	unsigned m = 42;

	ATF_CHECK_EQ(-1, pkgbe_parse_hooks("upgrade,bogus", &m));
	ATF_CHECK_EQ(-1, pkgbe_parse_hooks("upgrades", &m));
	ATF_CHECK_EQ(-1, pkgbe_parse_hooks("upgrade;install", &m));
	ATF_CHECK_EQ(42u, m);		/* unchanged on error */
	ATF_CHECK_EQ(-1, pkgbe_parse_hooks(NULL, &m));
	ATF_CHECK_EQ(-1, pkgbe_parse_hooks("upgrade", NULL));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, prune_keep_days);
	ATF_TP_ADD_TC(tp, prune_keep_days_never_newest);
	ATF_TP_ADD_TC(tp, name_time);
	ATF_TP_ADD_TC(tp, join_all_fit);
	ATF_TP_ADD_TC(tp, join_empty);
	ATF_TP_ADD_TC(tp, join_truncates);
	ATF_TP_ADD_TC(tp, join_first_too_long);
	ATF_TP_ADD_TC(tp, join_small_buffer);
	ATF_TP_ADD_TC(tp, props_parse);
	ATF_TP_ADD_TC(tp, props_parse_empty);
	ATF_TP_ADD_TC(tp, pkgdiff_basic);
	ATF_TP_ADD_TC(tp, pkgdiff_same_and_empty);
	ATF_TP_ADD_TC(tp, pkgdiff_versions_with_spaces_and_commas);
	ATF_TP_ADD_TC(tp, argv_rootdir);
	ATF_TP_ADD_TC(tp, is_slash);
	ATF_TP_ADD_TC(tp, hooks_parse);
	ATF_TP_ADD_TC(tp, hooks_parse_invalid);
	ATF_TP_ADD_TC(tp, make_name_basic);
	ATF_TP_ADD_TC(tp, make_name_seq);
	ATF_TP_ADD_TC(tp, make_name_truncation);
	ATF_TP_ADD_TC(tp, make_name_bad_args);
	ATF_TP_ADD_TC(tp, is_ours);
	ATF_TP_ADD_TC(tp, prune_keeps_newest);
	ATF_TP_ADD_TC(tp, prune_never_foreign);
	ATF_TP_ADD_TC(tp, prune_protects_active_nextboot);
	ATF_TP_ADD_TC(tp, prune_unlimited);
	ATF_TP_ADD_TC(tp, prune_seq_order);
	return (atf_no_error());
}
