/*-------------------------------------------------------------------------
 *
 * dynloader.h
 *	  dynamic loader for QNX4 using the shared library mechanism
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/qnx4.h,v 1.3 2001/03/01 19:03:44 petere Exp $
 *
 *	NOTES
 *
 *-------------------------------------------------------------------------
 */

#include "fmgr.h"

void	   *pg_dlopen(char *filename);
PGFunction	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror();
