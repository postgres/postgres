/*
 * These routines were taken from the Apache source, but were made
 * available with a PostgreSQL-compatible license.  Kudos Wilfredo
 * Sánchez <wsanchez@apple.com>.
 *
 * $Header: /cvsroot/pgsql/src/backend/port/dynloader/darwin.c,v 1.3 2000/11/14 21:26:21 petere Exp $
 */

#include "postgres.h"
#include <mach-o/dyld.h>
#include "dynloader.h"

void *pg_dlopen(const char *filename)
{
	NSObjectFileImage image;

	if (NSCreateObjectFileImageFromFile(filename, &image) !=
		NSObjectFileImageSuccess)
		return NULL;
	return NSLinkModule(image, filename, TRUE);
}

void pg_dlclose(void *handle)
{
	NSUnLinkModule(handle,FALSE);
	return;
}

PGFunction pg_dlsym(void *handle, const char *funcname)
{
	NSSymbol symbol;
	char *symname = (char*)malloc(strlen(funcname)+2);

	sprintf(symname, "_%s", funcname);
	symbol = NSLookupAndBindSymbol(symname);
	free(symname);
	return (PGFunction) NSAddressOfSymbol(symbol);
}

const char *pg_dlerror(void)
{
	return "no error message available";
}
