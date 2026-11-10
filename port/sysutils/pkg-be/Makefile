PORTNAME=	pkg-be
DISTVERSIONPREFIX=	v
DISTVERSION=	0.1.0
CATEGORIES=	sysutils

MAINTAINER=	mozgan@gmail.com
COMMENT=	Create ZFS boot environments automatically before pkg operations
WWW=		https://github.com/mozgan/pkg-be

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

USES=		uidfix
USE_GITHUB=	yes
GH_ACCOUNT=	mozgan

SUB_FILES=	pkg-message

MAKE_ARGS=	PREFIX=${PREFIX} LOCALBASE=${LOCALBASE} \
		MK_DEBUG_FILES=no

.include <bsd.port.mk>
