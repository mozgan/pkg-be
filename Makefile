# pkg-be: pkg(8) plugin that creates a ZFS boot environment before upgrades.

.include <bsd.own.mk>

PREFIX?=	/usr/local
LOCALBASE?=	/usr/local

SHLIB_NAME=	pkg-be.so
NO_SHLIB_LINKS=	yes
SHLIBDIR=	${PREFIX}/lib/pkg

.PATH:		${.CURDIR}/src
SRCS=		pkg-be.c cmd.c be_util.c

WARNS?=		6
CFLAGS+=	-I${.CURDIR}/include -I${LOCALBASE}/include
CFLAGS+=	-DPKG_CMD=\"${LOCALBASE}/sbin/pkg\"
LDFLAGS+=	-L${LOCALBASE}/lib
LDADD+=		-lpkg -lbe -lnvpair

MAN=		man/pkg-be.8
MANDIR=		${PREFIX}/share/man/man

CONFDIR=	${PREFIX}/etc/pkg

# pkg(8) does not create its plugin directory; staged installs start empty.
beforeinstall:
	${INSTALL} -d ${DESTDIR}${SHLIBDIR} ${DESTDIR}${MANDIR}8

afterinstall:
	${INSTALL} -d ${DESTDIR}${CONFDIR}
	${INSTALL} -m 0644 ${.CURDIR}/conf/pkg-be.conf.sample \
	    ${DESTDIR}${CONFDIR}/pkg-be.conf.sample

.include <bsd.lib.mk>

# Unit tests (ATF-C + Kyua, both in base).
# kyua needs its store schema (FreeBSD-tests on pkgbase systems); without
# it, run the test cases directly.
KYUA_SCHEMA?=	/usr/share/kyua/store/schema_v3.sql
test: .PHONY
	${MAKE} -C ${.CURDIR}/tests
.if exists(${KYUA_SCHEMA})
	cd ${.CURDIR}/tests && kyua test
.else
	cd ${.CURDIR}/tests && sh run_atf.sh ./be_util_test
.endif

# Static analysis (uses .clang-tidy). LLVM_VER: e.g. 19.
LLVM_VER?=	19
lint: .PHONY
	clang-tidy${LLVM_VER} --quiet ${.CURDIR}/src/*.c -- \
	    -I${.CURDIR}/include -I${.CURDIR}/src -I${LOCALBASE}/include
	mandoc -T lint -W warning ${.CURDIR}/man/pkg-be.8

# Integration test on a live ZFS system (needs root).
itest: .PHONY
	sh ${.CURDIR}/tests/integration.sh
