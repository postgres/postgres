/*-------------------------------------------------------------------------
 *
 * dynloader.c
 *	  Dynamic Loader for Postgres for BeOS
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/beos.c,v 1.7 2001/03/22 03:59:42 momjian Exp $
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
		/* Loading symbol */
		if (get_image_symbol(*((int *) (handle)), funcname, B_SYMBOL_TYPE_TEXT, (void **) &fpt) == B_OK);
		{

			/*
			 * Sometime the loader return B_OK for an inexistant function
			 * with an invalid address !!! Check that the return address
			 * is in the image range
			 */
			image_info	info;

			get_image_info(*((int *) (handle)), &info);
			if ((fpt < info.text) ||(fpt >= (info.text +info.text_size)))
				return NULL;
			return fpt;
		}
		elog(NOTICE, "loading symbol '%s' failed ", funcname);
	}
	elog(NOTICE, "add-on not loaded correctly");
	return NULL;
}

void
pg_dlclose(void *handle)
{
	/* Checking that "Handle" is valid */
	if ((handle) && ((*(int *) (handle)) >= 0))
	{
		if (beos_dl_close(*(image_id *) handle) != B_OK)
			elog(NOTICE, "error while unloading add-on");
		free(handle);
	}
}
