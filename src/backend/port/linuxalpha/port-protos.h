/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *	  port-specific prototypes for SunOS 4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.2 1997/09/07 04:46:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

#include "fmgr.h"				/* for func_ptr */
#include "utils/dynamic_loader.h"
#include "dlfcn.h"

#define pg_dlopen(f)	dlopen(f, 2)
#define pg_dlsym		dlsym
#define pg_dlclose		dlclose
#define pg_dlerror		dlerror

#endif							/* PORT_PROTOS_H */
