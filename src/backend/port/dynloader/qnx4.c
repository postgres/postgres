/*-------------------------------------------------------------------------
 *
 * dynloader.c
 *	  dynamic loader for QNX4 using the shared library mechanism
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/qnx4.c,v 1.4 2002/11/08 20:23:56 momjian Exp $
 *
 *	NOTES
 *
 *-------------------------------------------------------------------------
 */
/* System includes */

#include "postgres.h"

/*
#include <a.out.h>
#include <dl.h>
*/

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
	static char errmsg[] = "Failed to load shared library due to lack of shared library support.";

	return errmsg;
}
