/*-------------------------------------------------------------------------
 *
 * dynloader.c
 *	  Dynamic Loader for Postgres for BeOS
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/dynloader/Attic/beos.c,v 1.1 2000/10/02 17:15:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <kernel/OS.h>
#include <image.h>
#include <errno.h>

#include "dynloader.h"

extern char pg_pathname[];

void *
beos_dlopen(const char *filename)
{
    image_id id = -1;

	if ((id = load_add_on(filename)) < 0)
		return NULL;

	return (void *) id;
}

void 
beos_dlclose(void *handle)
{
    image_id id = (image_id) handle;
    unload_add_on(id);
    return;
}

void *
beos_dlsym(void *handle, const char *name)
{
    image_id id = (image_id)handle;
    void *addr;
    
    if (get_image_symbol(id, name, B_SYMBOL_TYPE_ANY, &addr) != B_OK)
        return NULL;
    
    return addr;
} 
        
char *
beos_dlerror()
{
    return (char *)strerror(errno);
}
