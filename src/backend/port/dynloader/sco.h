/*-------------------------------------------------------------------------
 *
 * port_protos.h
 *	  port-specific prototypes for SCO 3.2v5.2
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sco.h,v 1.7 2001/02/10 02:31:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include <dlfcn.h>
#include "utils/dynamic_loader.h"

/* dynloader.c */
/*
 * Dynamic Loader on SCO 3.2v5.0.2
 *
 * this dynamic loader uses the system dynamic loading interface for shared
 * libraries (ie. dlopen/dlsym/dlclose). The user must specify a shared
 * library as the file to be dynamically loaded.
 *
 */
#define pg_dlopen(f)	dlopen(f,1)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

/* port.c */

#endif	 /* PORT_PROTOS_H */
