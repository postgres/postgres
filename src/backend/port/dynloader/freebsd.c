/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
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
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)dl.c	5.4 (Berkeley) 2/23/91";

#endif	 /* LIBC_SCCS and not lint */

#include <sys/types.h>
#include <nlist.h>
#include <link.h>
#include <dlfcn.h>

#include <stdio.h>
#include <stdlib.h>

#include "dynloader.h"

static char error_message[BUFSIZ];

char *
BSD44_derived_dlerror(void)
{
	static char ret[BUFSIZ];

	strcpy(ret, error_message);
	error_message[0] = 0;
	return (ret[0] == 0) ? (char *) NULL : ret;
}

void *
BSD44_derived_dlopen(const char *file, int num)
{
#if defined(__mips__) || (defined(__NetBSD__) && defined(vax))
	sprintf(error_message, "dlopen (%s) not supported", file);
	return NULL;
#else
	void	   *vp;

	if ((vp = dlopen((char *) file, num)) == (void *) NULL)
		sprintf(error_message, "dlopen '%s' failed. (%s)", file, dlerror());
	return vp;
#endif
}

void *
BSD44_derived_dlsym(void *handle, const char *name)
{
#if defined(__mips__) || (defined(__NetBSD__) && defined(vax))
	sprintf(error_message, "dlsym (%s) failed", name);
	return NULL;
#else
	void	   *vp;
	char		buf[BUFSIZ];

#ifndef __ELF__
	if (*name != '_')
	{
		sprintf(buf, "_%s", name);
		name = buf;
	}
#endif
	if ((vp = dlsym(handle, (char *) name)) == (void *) NULL)
		sprintf(error_message, "dlsym (%s) failed", name);
	return vp;
#endif
}

void
BSD44_derived_dlclose(void *handle)
{
#if defined(__mips__) || (defined(__NetBSD__) && defined(vax))
#else
	dlclose(handle);
#endif
}
