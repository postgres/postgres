/* $PostgreSQL: pgsql/src/backend/port/dynloader/nextstep.c,v 1.6 2006/03/11 04:38:31 momjian Exp $ */

#include "postgres.h"

#include "mach-o/rld.h"
#include "streams/streams.h"

static char *lastError = NULL;

static NXStream *
OpenError()
{
	return NXOpenMemory(NULL, 0, NX_WRITEONLY);
}

static void
CloseError(NXStream * s)
{
	if (s)
		NXCloseMemory(s, NX_FREEBUFFER);
}

static void
TransferError(NXStream * s)
{
	char	   *buffer;
	int			len,
				maxlen;

	if (lastError)
		free(lastError);
	NXGetMemoryBuffer(s, &buffer, &len, &maxlen);
	lastError = malloc(len + 1);
	strcpy(lastError, buffer);
}

void *
next_dlopen(char *name)
{
	int			rld_success;
	NXStream   *errorStream;
	char	   *result = NULL;
	char	  **p;

	errorStream = OpenError();
	p = calloc(2, sizeof(void *));
	p[0] = name;
	rld_success = rld_load(errorStream, NULL, p, NULL);
	free(p);

	if (!rld_success)
	{
		TransferError(errorStream);
		result = (char *) 1;
	}
	CloseError(errorStream);
	return result;
}

int
next_dlclose(void *handle)
{
	return 0;
}

void *
next_dlsym(void *handle, char *symbol)
{
	NXStream   *errorStream = OpenError();
	char		symbuf[1024];
	unsigned long symref = 0;

	snprintf(symbuf, sizeof(symbuf), "_%s", symbol);
	if (!rld_lookup(errorStream, symbuf, &symref))
		TransferError(errorStream);
	CloseError(errorStream);
	return (void *) symref;
}

char *
next_dlerror(void)
{
	return lastError;
}
