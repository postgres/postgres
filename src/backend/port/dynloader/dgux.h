/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *	  port-specific prototypes for SunOS 4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dgux.h,v 1.1 1997/12/20 04:48:04 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "fmgr.h"				/* for func_ptr */
#include "utils/dynamic_loader.h"
#include "dlfcn.h"

/* dynloader.c */

/* #define		pg_dlopen(f)	dlopen(f, 1) */
#define pg_dlopen(f)	dlopen(f, 2)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

/* port.c */

#endif							/* PORT_PROTOS_H */
