/*-------------------------------------------------------------------------
 *
 * dynloader.h--
 *	  dynamic loader for HP-UX using the shared library mechanism
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/hpux.h,v 1.1 1998/01/26 02:48:36 scrappy Exp $
 *
 *	NOTES
 *		all functions are defined here -- it's impossible to trace the
 *		shl_* routines from the bundled HP-UX debugger.
 *
 *-------------------------------------------------------------------------
 */
/* System includes */
void	   *pg_dlopen(char *filename);
func_ptr	pg_dlsym(void *handle, char *funcname);
void		pg_dlclose(void *handle);
char	   *pg_dlerror();
