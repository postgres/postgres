/*-------------------------------------------------------------------------
 *
 * mkdtemp.c
 *	  create a mode-0700 temporary directory
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/mkdtemp.c
 *
 * This code was taken from NetBSD to provide an implementation for platforms
 * that lack it.  (Among compatibly-licensed implementations, the OpenBSD
 * version better resists denial-of-service attacks.  However, it has a
 * cryptographic dependency.)  The NetBSD copyright terms follow.
 *-------------------------------------------------------------------------
 */

#include "c.h"

#define _DIAGASSERT(x) do {} while (0)


/*	$NetBSD: gettemp.c,v 1.17 2014/01/21 19:09:48 seanb Exp $	*/

/*
 * Copyright (c) 1987, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if HAVE_NBTOOL_CONFIG_H
#include "nbtool_config.h"
#endif

#if !HAVE_NBTOOL_CONFIG_H || !HAVE_MKSTEMP || !HAVE_MKDTEMP

#ifdef NOT_POSTGRESQL
#include <sys/cdefs.h>
#if defined(LIBC_SCCS) && !defined(lint)
#if 0
static char sccsid[] = "@(#)mktemp.c	8.1 (Berkeley) 6/4/93";
#else
__RCSID("$NetBSD: gettemp.c,v 1.17 2014/01/21 19:09:48 seanb Exp $");
#endif
#endif   /* LIBC_SCCS and not lint */
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef NOT_POSTGRESQL
#if HAVE_NBTOOL_CONFIG_H
#define GETTEMP		__nbcompat_gettemp
#else
#include "reentrant.h"
#include "local.h"
#define GETTEMP		__gettemp
#endif
#endif

static int
GETTEMP(char *path, int *doopen, int domkdir)
{
	char	   *start,
			   *trv;
	struct stat sbuf;
	u_int		pid;

	/*
	 * To guarantee multiple calls generate unique names even if the file is
	 * not created. 676 different possibilities with 7 or more X's, 26 with 6
	 * or less.
	 */
	static char xtra[2] = "aa";
	int			xcnt = 0;

	_DIAGASSERT(path != NULL);
	/* doopen may be NULL */

	pid = getpid();

	/* Move to end of path and count trailing X's. */
	for (trv = path; *trv; ++trv)
		if (*trv == 'X')
			xcnt++;
		else
			xcnt = 0;

	/* Use at least one from xtra.  Use 2 if more than 6 X's. */
	if (xcnt > 0)
	{
		*--trv = xtra[0];
		xcnt--;
	}
	if (xcnt > 5)
	{
		*--trv = xtra[1];
		xcnt--;
	}

	/* Set remaining X's to pid digits with 0's to the left. */
	for (; xcnt > 0; xcnt--)
	{
		*--trv = (pid % 10) + '0';
		pid /= 10;
	}

	/* update xtra for next call. */
	if (xtra[0] != 'z')
		xtra[0]++;
	else
	{
		xtra[0] = 'a';
		if (xtra[1] != 'z')
			xtra[1]++;
		else
			xtra[1] = 'a';
	}

	/*
	 * check the target directory; if you have six X's and it doesn't exist
	 * this runs for a *very* long time.
	 */
	for (start = trv + 1;; --trv)
	{
		if (trv <= path)
			break;
		if (*trv == '/')
		{
			int			e;

			*trv = '\0';
			e = stat(path, &sbuf);
			*trv = '/';
			if (e == -1)
				return doopen == NULL && !domkdir;
			if (!S_ISDIR(sbuf.st_mode))
			{
				errno = ENOTDIR;
				return doopen == NULL && !domkdir;
			}
			break;
		}
	}

	for (;;)
	{
		if (doopen)
		{
			if ((*doopen =
				 open(path, O_CREAT | O_EXCL | O_RDWR, 0600)) >= 0)
				return 1;
			if (errno != EEXIST)
				return 0;
		}
		else if (domkdir)
		{
			if (mkdir(path, 0700) >= 0)
				return 1;
			if (errno != EEXIST)
				return 0;
		}
		else if (lstat(path, &sbuf))
			return errno == ENOENT ? 1 : 0;

		/* tricky little algorithm for backward compatibility */
		for (trv = start;;)
		{
			if (!*trv)
				return 0;
			if (*trv == 'z')
				*trv++ = 'a';
			else
			{
				if (isdigit((unsigned char) *trv))
					*trv = 'a';
				else
					++* trv;
				break;
			}
		}
	}
	/* NOTREACHED */
}

#endif   /* !HAVE_NBTOOL_CONFIG_H || !HAVE_MKSTEMP ||
								 * !HAVE_MKDTEMP */


/*	$NetBSD: mkdtemp.c,v 1.11 2012/03/15 18:22:30 christos Exp $	*/

/*
 * Copyright (c) 1987, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if HAVE_NBTOOL_CONFIG_H
#include "nbtool_config.h"
#endif

#if !HAVE_NBTOOL_CONFIG_H || !HAVE_MKDTEMP

#ifdef NOT_POSTGRESQL

#include <sys/cdefs.h>
#if defined(LIBC_SCCS) && !defined(lint)
#if 0
static char sccsid[] = "@(#)mktemp.c	8.1 (Berkeley) 6/4/93";
#else
__RCSID("$NetBSD: mkdtemp.c,v 1.11 2012/03/15 18:22:30 christos Exp $");
#endif
#endif   /* LIBC_SCCS and not lint */

#if HAVE_NBTOOL_CONFIG_H
#define GETTEMP		__nbcompat_gettemp
#else
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "reentrant.h"
#include "local.h"
#define GETTEMP		__gettemp
#endif

#endif

char *
mkdtemp(char *path)
{
	_DIAGASSERT(path != NULL);

	return GETTEMP(path, NULL, 1) ? path : NULL;
}

#endif   /* !HAVE_NBTOOL_CONFIG_H || !HAVE_MKDTEMP */
