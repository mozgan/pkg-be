/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Mehmet Ozgan <mozgan@gmail.com>
 *
 * <be.h> includes <sys/nvpair.h>, which needs Solaris types that base
 * normally gets from OpenZFS libspl headers (not installed in /usr/include).
 * Provide the few that nvpair.h uses.
 */
#ifndef PKG_BE_COMPAT_H
#define PKG_BE_COMPAT_H

#include <sys/types.h>

typedef unsigned int	uint_t;
typedef unsigned char	uchar_t;
typedef long long	hrtime_t;
typedef enum { B_FALSE = 0, B_TRUE = 1 } boolean_t;

#include <be.h>

#endif
