/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/lib/Attic/memory.c,v 1.7 2001/11/14 11:11:49 meskes Exp $ */

#include "postgres_fe.h"

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"

char *
ECPGalloc(long size, int lineno)
{
	char	   *new = (char *) calloc(1L, size);

	if (!new)
	{
		ECPGraise(lineno, ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	memset(new, '\0', size);
	return (new);
}

char *
ECPGstrdup(const char *string, int lineno)
{
	char	   *new = strdup(string);

	if (!new)
	{
		ECPGraise(lineno, ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return (new);
}
