/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/lib/Attic/memory.c,v 1.4 2001/08/24 14:07:49 petere Exp $ */

#include "postgres_fe.h"

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"

char *
ecpg_alloc(long size, int lineno)
{
	char	   *new = (char *) calloc(1L, size);

	if (!new)
	{
		ECPGlog("out of memory\n");
		ECPGraise(lineno, ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	memset(new, '\0', size);
	return (new);
}

char *
ecpg_strdup(const char *string, int lineno)
{
	char	   *new = strdup(string);

	if (!new)
	{
		ECPGlog("out of memory\n");
		ECPGraise(lineno, ECPG_OUT_OF_MEMORY, NULL);
		return NULL;
	}

	return (new);
}
