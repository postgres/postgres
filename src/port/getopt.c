/* src/port/getopt.c */

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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "c.h"

#include "pg_getopt.h"
#include "port/pg_getopt_ctx.h"

/*
 * On OpenBSD and some versions of Solaris, opterr and friends are defined in
 * core libc rather than in a separate getopt module.  Define these variables
 * only if configure found they aren't there by default; otherwise, this
 * module and its callers will just use libc's variables.  (We assume that
 * testing opterr is sufficient for all of these.)
 */
#ifndef HAVE_INT_OPTERR

int			opterr = 1,			/* if error message should be printed */
			optind = 1,			/* index into parent argv vector */
			optopt;				/* character checked for validity */
char	   *optarg;				/* argument associated with option */

#endif

/*
 * getopt
 *	Parse argc/argv argument vector.
 *
 * We use the re-entrant pg_getopt_start/next() functions under the hood, but
 * expose the standard non re-entrant API.
 *
 * This implementation does not use optreset.  Instead, we guarantee that
 * it can be restarted on a new argv array after a previous call returned -1,
 * if the caller resets optind to 1 before the first call of the new series.
 * (Internally, this means we must be sure to reset "active" before
 * returning -1.)
 */
int
getopt(int nargc, char *const *nargv, const char *ostr)
{
	static bool active = false;
	static pg_getopt_ctx ctx;
	int			result;

	if (!active)
	{
		pg_getopt_start(&ctx, nargc, nargv, ostr);
		ctx.opterr = opterr;
		active = true;
	}

	result = pg_getopt_next(&ctx);
	opterr = ctx.opterr;
	optind = ctx.optind;
	optopt = ctx.optopt;
	optarg = ctx.optarg;
	if (result == -1)
		active = false;
	return result;
}
