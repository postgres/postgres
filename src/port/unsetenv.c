/*-------------------------------------------------------------------------
 *
 * unsetenv.c
 *	  unsetenv() emulation for machines without it
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/unsetenv.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"


void
unsetenv(const char *name)
{
	char	   *envstr;

	if (getenv(name) == NULL)
		return;					/* no work */

	/*
	 * The technique embodied here works if libc follows the Single Unix Spec
	 * and actually uses the storage passed to putenv() to hold the environ
	 * entry.  When we clobber the entry in the second step we are ensuring
	 * that we zap the actual environ member.  However, there are some libc
	 * implementations (notably recent BSDs) that do not obey SUS but copy the
	 * presented string.  This method fails on such platforms.  Hopefully all
	 * such platforms have unsetenv() and thus won't be using this hack. See:
	 * http://www.greenend.org.uk/rjk/2008/putenv.html
	 *
	 * Note that repeatedly setting and unsetting a var using this code will
	 * leak memory.
	 */

	envstr = (char *) malloc(strlen(name) + 2);
	if (!envstr)				/* not much we can do if no memory */
		return;

	/* Override the existing setting by forcibly defining the var */
	sprintf(envstr, "%s=", name);
	putenv(envstr);

	/* Now we can clobber the variable definition this way: */
	strcpy(envstr, "=");

	/*
	 * This last putenv cleans up if we have multiple zero-length names as a
	 * result of unsetting multiple things.
	 */
	putenv(envstr);
}
