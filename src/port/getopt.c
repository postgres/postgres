/* src/port/getopt.c */

/* This is used by psql under Win32 */

/*
 * Copyright (c) 1987, 1993, 1994
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
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 */

#include "c.h"

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)getopt.c	8.3 (Berkeley) 4/27/95";
#endif   /* LIBC_SCCS and not lint */


/*
 * On some versions of Solaris, opterr and friends are defined in core libc
 * rather than in a separate getopt module.  Define these variables only
 * if configure found they aren't there by default.  (We assume that testing
 * opterr is sufficient for all of these.)
 */
#ifndef HAVE_INT_OPTERR

int			opterr = 1,			/* if error message should be printed */
			optind = 1,			/* index into parent argv vector */
			optopt;				/* character checked for validity */
char	   *optarg;				/* argument associated with option */
#else

extern int	opterr;
extern int	optind;
extern int	optopt;
extern char *optarg;
#endif

#define BADCH	(int)'?'
#define BADARG	(int)':'
#define EMSG	""

int			getopt(int nargc, char *const * nargv, const char *ostr);

/*
 * getopt
 *	Parse argc/argv argument vector.
 *
 * This implementation does not use optreset.  Instead, we guarantee that
 * it can be restarted on a new argv array after a previous call returned -1,
 * if the caller resets optind to 1 before the first call of the new series.
 * (Internally, this means we must be sure to reset "place" to EMSG before
 * returning -1.)
 */
int
getopt(int nargc, char *const * nargv, const char *ostr)
{
	static char *place = EMSG;	/* option letter processing */
	char	   *oli;			/* option letter list index */

	if (!*place)
	{							/* update scanning pointer */
		if (optind >= nargc || *(place = nargv[optind]) != '-')
		{
			place = EMSG;
			return -1;
		}
		if (place[1] && *++place == '-' && place[1] == '\0')
		{						/* found "--" */
			++optind;
			place = EMSG;
			return -1;
		}
	}							/* option letter okay? */
	if ((optopt = (int) *place++) == (int) ':' ||
		!(oli = strchr(ostr, optopt)))
	{
		/*
		 * if the user didn't specify '-' as an option, assume it means -1.
		 */
		if (optopt == (int) '-')
		{
			place = EMSG;
			return -1;
		}
		if (!*place)
			++optind;
		if (opterr && *ostr != ':')
			(void) fprintf(stderr,
						   "illegal option -- %c\n", optopt);
		return BADCH;
	}
	if (*++oli != ':')
	{							/* don't need argument */
		optarg = NULL;
		if (!*place)
			++optind;
	}
	else
	{							/* need an argument */
		if (*place)				/* no white space */
			optarg = place;
		else if (nargc <= ++optind)
		{						/* no arg */
			place = EMSG;
			if (*ostr == ':')
				return BADARG;
			if (opterr)
				(void) fprintf(stderr,
							   "option requires an argument -- %c\n",
							   optopt);
			return BADCH;
		}
		else
			/* white space */
			optarg = nargv[optind];
		place = EMSG;
		++optind;
	}
	return optopt;				/* dump back option letter */
}
