# Unit tests for the pure helpers in src/be_util.c.
.PATH:		${.CURDIR}/../src

PROG=		be_util_test
SRCS=		be_util_test.c be_util.c
MAN=
WARNS?=		6
CFLAGS+=	-I${.CURDIR}/../src

# Prefer devel/atf (ports); fall back to base's private copy.
LOCALBASE?=	/usr/local
.if exists(${LOCALBASE}/lib/libatf-c.so)
CFLAGS+=	-I${LOCALBASE}/include
LDFLAGS+=	-L${LOCALBASE}/lib -Wl,-rpath,${LOCALBASE}/lib
LDADD+=		-latf-c
.else
LDADD+=		-lprivateatf-c
.endif

.include <bsd.prog.mk>
