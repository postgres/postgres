/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *	  port-specific prototypes for AIX
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.2 1997/09/07 04:45:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "dlfcn.h"				/* this is from jum's libdl package */

/* dynloader.c */

#define  pg_dlopen(f)	dlopen(filename, RTLD_LAZY)
#define  pg_dlsym(h,f)	dlsym(h, f)
#define  pg_dlclose(h)	dlclose(h)
#define  pg_dlerror()	dlerror()

#endif							/* PORT_PROTOS_H */
