/*-------------------------------------------------------------------------
 *
 * port_protos.h
 *	  port-specific prototypes for BeOS
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: beos.h,v 1.1 2000/10/02 17:15:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "postgres.h"

#include "fmgr.h"
#include "utils/dynamic_loader.h"

char	   *beos_dlerror(void);
void	   *beos_dlopen(const char *filename);
void	   *beos_dlsym(void *handle, const char *name);
void		beos_dlclose(void *handle);

#define		   pg_dlopen(f)    beos_dlopen(f)
#define		   pg_dlsym		   beos_dlsym
#define		   pg_dlclose	   beos_dlclose
#define		   pg_dlerror	   beos_dlerror


#endif	 /* PORT_PROTOS_H */
