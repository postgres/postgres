#include <ecpgtype.h>
#include <ecpglib.h>

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
