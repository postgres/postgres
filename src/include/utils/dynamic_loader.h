/*-------------------------------------------------------------------------
 *
 * dynamic_loader.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dynamic_loader.h,v 1.10 1999/02/13 23:22:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_LOADER_H
#define DYNAMIC_LOADER_H

#include <sys/types.h>
#include <sys/param.h>			/* For MAXPATHLEN */

#include <postgres.h>

#ifdef MIN
#undef MIN
#undef MAX
#endif	 /* MIN */

/*
 * List of dynamically loaded files.
 */

typedef struct df_files
{
	char		filename[MAXPATHLEN];	/* Full pathname of file */
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
