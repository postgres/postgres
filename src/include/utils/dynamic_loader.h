/*-------------------------------------------------------------------------
 *
 * dynamic_loader.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dynamic_loader.h,v 1.2 1996/11/04 08:14:05 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_LOADER_H
#define DYNAMIC_LOADER_H

#include <sys/types.h>

#ifdef MIN
#undef MIN
#undef MAX
#endif /* MIN */

#ifdef WIN32
#define MAXPATHLEN    250
#endif

#ifdef WIN32
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
