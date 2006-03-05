/*-------------------------------------------------------------------------
 *
 * dynamic_loader.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/dynamic_loader.h,v 1.26 2006/03/05 15:59:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_LOADER_H
#define DYNAMIC_LOADER_H

#include "fmgr.h"


extern void *pg_dlopen(char *filename);
extern PGFunction pg_dlsym(void *handle, char *funcname);
extern void pg_dlclose(void *handle);
extern char *pg_dlerror(void);

#endif   /* DYNAMIC_LOADER_H */
