/*-------------------------------------------------------------------------
 *
 * smgrtype.c--
 *	  storage manager type
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/smgrtype.c,v 1.4 1997/09/08 02:29:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "utils/builtins.h"		/* where the declarations go */
#include "utils/palloc.h"
#include "storage/smgr.h"

typedef struct smgrid
{
	char	   *smgr_name;
}			smgrid;

/*
 *	StorageManager[] -- List of defined storage managers.
 *
 *		The weird comma placement is to keep compilers happy no matter
 *		which of these is (or is not) defined.
 */

static smgrid StorageManager[] = {
	{"magnetic disk"},
#ifdef MAIN_MEMORY
	{"main memory"}
#endif							/* MAIN_MEMORY */
};

static int	NStorageManagers = lengthof(StorageManager);

int2
smgrin(char *s)
{
	int			i;

	for (i = 0; i < NStorageManagers; i++)
	{
		if (strcmp(s, StorageManager[i].smgr_name) == 0)
			return ((int2) i);
	}
	elog(WARN, "smgrin: illegal storage manager name %s", s);
	return 0;
}

char	   *
smgrout(int2 i)
{
	char	   *s;

	if (i >= NStorageManagers || i < 0)
		elog(WARN, "Illegal storage manager id %d", i);

	s = (char *) palloc(strlen(StorageManager[i].smgr_name) + 1);
	strcpy(s, StorageManager[i].smgr_name);
	return (s);
}

bool
smgreq(int2 a, int2 b)
{
	if (a == b)
		return (true);
	return (false);
}

bool
smgrne(int2 a, int2 b)
{
	if (a == b)
		return (false);
	return (true);
}
