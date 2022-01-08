/*-------------------------------------------------------------------------
 *
 * setenv.c
 *	  setenv() emulation for machines without it
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/setenv.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"


int
setenv(const char *name, const char *value, int overwrite)
{
	char	   *envstr;

	/* Error conditions, per POSIX */
	if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL ||
		value == NULL)
	{
		errno = EINVAL;
		return -1;
	}

	/* No work if variable exists and we're not to replace it */
	if (overwrite == 0 && getenv(name) != NULL)
		return 0;

	/*
	 * Add or replace the value using putenv().  This will leak memory if the
	 * same variable is repeatedly redefined, but there's little we can do
	 * about that when sitting atop putenv().
	 */
	envstr = (char *) malloc(strlen(name) + strlen(value) + 2);
	if (!envstr)				/* not much we can do if no memory */
		return -1;

	sprintf(envstr, "%s=%s", name, value);

	return putenv(envstr);
}
