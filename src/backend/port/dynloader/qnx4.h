/*-------------------------------------------------------------------------
 *
 * dynloader.h
 *	  dynamic loader for QNX4 using the shared library mechanism
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/qnx4.h,v 1.1 1999/12/16 01:25:04 momjian Exp $
 *
 *	NOTES
 *
 *-------------------------------------------------------------------------
 */
/* System includes */
void	   *pg_dlopen(char *filename);
func_ptr	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror();
