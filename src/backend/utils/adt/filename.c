/*-------------------------------------------------------------------------
 *
 * filename.c--
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/filename.c,v 1.11 1997/10/25 01:10:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>
#include <stdio.h>
#include <pwd.h>

#include <sys/param.h>

#include "postgres.h"
#include <miscadmin.h>
#include "utils/builtins.h"		/* where function declarations go */

char	   *
filename_in(char *file)
{
	char	   *str;
	int			ind = 0;

	/*
	 * XXX - HACK CITY --- REDO should let the shell do expansions
	 * (shexpand)
	 */

	str = (char *) palloc(MAXPATHLEN * sizeof(*str));
	str[0] = '\0';
	if (file[0] == '~')
	{
		if (file[1] == '\0' || file[1] == '/')
		{
			/* Home directory */

			char	   *userName;
			struct passwd *pw;

			userName = GetPgUserName();

			if ((pw = getpwnam(userName)) == NULL)
			{
				elog(WARN, "User %s is not a Unix user on the db server.",
					 userName);
			}

			strcpy(str, pw->pw_dir);

			ind = 1;
		}
		else
		{
			/* Someone else's directory */
			char		name[17],
					   *p;
			struct passwd *pw;
			int			len;

			if ((p = (char *) strchr(file, '/')) == NULL)
			{
				strcpy(name, file + 1);
				len = strlen(name);
			}
			else
			{
				len = (p - file) - 1;
				StrNCpy(name, file + 1, len+1);
			}
			/* printf("name: %s\n"); */
			if ((pw = getpwnam(name)) == NULL)
			{
				elog(WARN, "No such user: %s\n", name);
				ind = 0;
			}
			else
			{
				strcpy(str, pw->pw_dir);
				ind = len + 1;
			}
		}
	}
	else if (file[0] == '$')
	{							/* $POSTGRESHOME, etc.	expand it. */
		char		environment[80],
				   *envirp,
				   *p;
		int			len;

		if ((p = (char *) strchr(file, '/')) == NULL)
		{
			strcpy(environment, file + 1);
			len = strlen(environment);
		}
		else
		{
			len = (p - file) - 1;
			StrNCpy(environment, file + 1, len+1);
		}
		envirp = getenv(environment);
		if (envirp)
		{
			strcpy(str, envirp);
			ind = len + 1;
		}
		else
		{
			elog(WARN, "Couldn't find %s in your environment", environment);
		}
	}
	else
	{
		ind = 0;
	}
	strcat(str, file + ind);
	return (str);
}

char	   *
filename_out(char *s)
{
	char	   *ret;

	if (!s)
		return ((char *) NULL);
	ret = (char *) palloc(strlen(s) + 1);
	if (!ret)
		elog(WARN, "filename_out: palloc failed");
	return (strcpy(ret, s));
}
