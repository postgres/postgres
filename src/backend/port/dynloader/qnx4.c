/*-------------------------------------------------------------------------
 *
 * dynloader.c
 *	  dynamic loader for QNX4 using the shared library mechanism
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/qnx4.c,v 1.2 2000/05/28 17:56:02 tgl Exp $
 *
 *	NOTES
 *
 *-------------------------------------------------------------------------
 */
/* System includes */
/*
#include <stdio.h>
#include <a.out.h>
#include <dl.h>
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
	static char errmsg[] = "Failed to load shared library due to lack of shared library support.";

	return errmsg;
}
