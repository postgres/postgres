/*
 * This is a place holder until someone supplies a dynamic loader
 * interface for this platform.
 *
 * $Header: /cvsroot/pgsql/src/backend/port/dynloader/darwin.c,v 1.1 2000/10/31 19:55:19 petere Exp $
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/dynamic_loader.h"
#include "dynloader.h"

void *
pg_dlopen(char *filename)
{
	return (void *) NULL;
}

PGFunction
pg_dlsym(void *handle, char *funcname)
{
	return NULL;
}

void
pg_dlclose(void *handle)
{
}

char *
pg_dlerror()
{
	static char errmsg[] = "the dynamic loader for darwin doesn't exist yet";

	return errmsg;
}
