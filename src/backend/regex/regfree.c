/*-
 * Copyright (c) 1992, 1993, 1994 Henry Spencer.
 * Copyright (c) 1992, 1993, 1994
 *		The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Henry Spencer.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *		This product includes software developed by the University of
 *		California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 * SUCH DAMAGE.
 *
 *		@(#)regfree.c	8.3 (Berkeley) 3/20/94
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)regfree.c	8.3 (Berkeley) 3/20/94";

#endif	 /* LIBC_SCCS and not lint */

#include "postgres.h"

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

#include "regex/regex.h"
#include "regex/utils.h"
#include "regex/regex2.h"

/*
 - regfree - free everything
 = extern void regfree(regex_t *);
 */
void
pg95_regfree(preg)
regex_t    *preg;
{
	struct re_guts *g;

	if (preg->re_magic != MAGIC1)		/* oops */
		return;					/* nice to complain, but hard */

	g = preg->re_g;
	if (g == NULL || g->magic != MAGIC2)		/* oops again */
		return;
	preg->re_magic = 0;			/* mark it invalid */
	g->magic = 0;				/* mark it invalid */
#ifdef MULTIBYTE
	if (preg->patsave != NULL)
		free((char *) preg->patsave);
#endif
	if (g->strip != NULL)
		free((char *) g->strip);
	if (g->sets != NULL)
		free((char *) g->sets);
	if (g->setbits != NULL)
		free((char *) g->setbits);
	if (g->must != NULL)
		free(g->must);
	free((char *) g);
}
