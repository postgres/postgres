/*-------------------------------------------------------------------------
 *
 * dynloader.c
 *	  Dynamic Loader for Postgres for BeOS
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/beos.c,v 1.12 2003/08/04 02:40:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/dynamic_loader.h"


void *
pg_dlopen(char *filename)
{
	image_id   *im;

	/* Handle memory allocation to store the Id of the shared object */
	im = (image_id *) (malloc(sizeof(image_id)));

	/* Add-on loading */
	*im = beos_dl_open(filename);

	return im;
}


char *
pg_dlerror()
{
	static char errmsg[] = "Load Add-On failed";

	return errmsg;
}

PGFunction
pg_dlsym(void *handle, char *funcname)
{
	PGFunction	fpt;

	/* Checking that "Handle" is valid */
	if ((handle) && ((*(int *) (handle)) >= 0))
	{
		beos_dl_sym(*((int *) (handle)), funcname, (void **) &fpt);
		return fpt;
	}
	elog(WARNING, "add-on not loaded correctly");
	return NULL;
}

void
pg_dlclose(void *handle)
{
	/* Checking that "Handle" is valid */
	if ((handle) && ((*(int *) (handle)) >= 0))
	{
		if (beos_dl_close(*(image_id *) handle) != B_OK)
			elog(WARNING, "error while unloading add-on");
		free(handle);
	}
}
