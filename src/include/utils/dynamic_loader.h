/*-------------------------------------------------------------------------
 *
 * dynamic_loader.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dynamic_loader.h,v 1.1 1996/08/28 01:58:49 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_LOADER_H
#define DYNAMIC_LOADER_H

#ifdef MIN
#undef MIN
#undef MAX
#endif /* MIN */

#ifdef WIN32
#define MAXPATHLEN    250
#endif

#include <sys/param.h>			/* for MAXPATHLEN */
#include <sys/types.h>			/* for dev_t, ino_t, etc. */
#ifdef WIN32
#include <wchar.h>
#endif

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
