/*
 * $Header: /cvsroot/pgsql/src/port/Attic/opendir.c,v 1.1 2003/05/09 01:16:29 momjian Exp $
 *
 * Copyright (c) 2003 SRA, Inc.
 * Copyright (c) 2003 SKC, Inc.
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include <stddef.h>
#include <sys/types.h>
#include <windows.h>

#include "dirent.h"

DIR *
opendir(const char *dir)
{
	DIR			*dp;
	char		*findspec;
	HANDLE		handle;
	size_t		dirlen;

	dirlen = strlen(dir);
	findspec = palloc(dirlen + 2 + 1);
	if (findspec == NULL)
		return NULL;

	if (dirlen == 0)
		strcpy(findspec, "*");
	else if (isalpha(dir[0]) && dir[1] == ':' && dir[2] == '\0')
		sprintf(findspec, "%s*", dir);
	else if (dir[dirlen - 1] == '/' || dir[dirlen - 1] == '\\')
		sprintf(findspec, "%s*", dir);
	else
		sprintf(findspec, "%s\\*", dir);

	dp = (DIR *)palloc(sizeof(DIR));
	if (dp == NULL)
	{
		pfree(findspec);
		errno = ENOMEM;
		return NULL;
	}

	dp->offset = 0;
	dp->finished = 0;
	dp->dir = pstrdup(dir);
	if (dp->dir == NULL)
	{
		pfree(dp);
		pfree(findspec);
		errno = ENOMEM;
		return NULL;
	}

	handle = FindFirstFile(findspec, &(dp->finddata));
	if (handle == INVALID_HANDLE_VALUE)
	{
		pfree(dp->dir);
		pfree(dp);
		pfree(findspec);
		errno = ENOENT;
		return NULL;
	}
	dp->handle = handle;

	pfree(findspec);
	return dp;
}


struct dirent *
readdir(DIR *dp)
{
	if (dp == NULL || dp->finished)
		return NULL;

	if (dp->offset != 0)
	{
		if (FindNextFile(dp->handle, &(dp->finddata)) == 0)
		{
			dp->finished = 1;
			return NULL;
		}
	}
	dp->offset++;

	strncpy(dp->ent.d_name, dp->finddata.cFileName, MAX_PATH);
	dp->ent.d_ino = 1;

	return &(dp->ent);
}


int
closedir(DIR *dp)
{
	FindClose(dp->handle);
	pfree(dp->dir);
	pfree(dp);

	return 0;
}
