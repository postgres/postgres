/*-------------------------------------------------------------------------
 *
 * dynamic_loader.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dynamic_loader.h,v 1.4 1996/12/28 02:12:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_LOADER_H
#define DYNAMIC_LOADER_H

#include <sys/types.h>
#include <sys/param.h>	/* For MAXPATHLEN */

#include <postgres.h>

#ifdef MIN
#undef MIN
#undef MAX
#endif /* MIN */

/*
 * List of dynamically loaded files.
 */

typedef struct df_files {
    char filename[MAXPATHLEN];		/* Full pathname of file */
#ifdef WIN32
    _dev_t device;			/* Device file is on */
    _ino_t inode;			/* Inode number of file */
#else
    dev_t device;			/* Device file is on */
    ino_t inode;			/* Inode number of file */
#endif /* WIN32 */
    void *handle;			/* a handle for pg_dl* functions */
    struct df_files *next;
} DynamicFileList;

extern void *pg_dlopen(char *filename);
extern func_ptr pg_dlsym(void *handle, char *funcname);
extern void pg_dlclose(void *handle);
extern char *pg_dlerror(void);

#endif	/* DYNAMIC_LOADER_H */
