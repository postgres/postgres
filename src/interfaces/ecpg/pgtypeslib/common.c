#include <errno.h>
#include <stdlib.h>

#include "extern.h"
	
char *
pgtypes_alloc(long size)
{
	char *new = (char *) calloc(1L, size);

	if (!new)
	{
		errno = ENOMEM;
		return NULL;
	}

	memset(new, '\0', size);
	return (new);
}

char *
pgtypes_strdup(char *str)
{
	char *new = (char *) strdup(str);

	if (!new)
		errno = ENOMEM;
	return (new);
}

