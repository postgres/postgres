/*-------------------------------------------------------------------------
 *
 * dynamic_loader.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dynamic_loader.h,v 1.14 2000/01/26 05:58:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_LOADER_H
#define DYNAMIC_LOADER_H

#include <sys/types.h>

/* we need this include because port files use them */
#include "postgres.h"

#ifdef MIN
#undef MIN
#undef MAX
#endif	 /* MIN */

/*
 * List of dynamically loaded files.
 */

typedef struct df_files
{
	char		filename[MAXPGPATH];	/* Full pathname of file */
	dev_t		device;			/* Device file is on */
	ino_t		inode;			/* Inode number of file */
	void	   *handle;			/* a handle for pg_dl* functions */
	struct df_files *next;
} DynamicFileList;

extern void *pg_dlopen(char *filename);
extern func_ptr pg_dlsym(void *handle, char *funcname);
extern void pg_dlclose(void *handle);
extern char *pg_dlerror(void);

#endif	 /* DYNAMIC_LOADER_H */
