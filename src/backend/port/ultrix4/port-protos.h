/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *    prototypes for Ultrix-specific routines
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: port-protos.h,v 1.2 1996/11/26 03:18:58 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORT_PORTOS_H
#define PORT_PORTOS_H

/* dynloader.c */
/*
 * New dynamic loader.
 *
 * This dynamic loader uses Andrew Yu's libdl-1.0 package for Ultrix 4.x.
 * (Note that pg_dlsym and pg_dlclose are actually macros defined in
 * "port-protos.h".)
 */ 

#define pg_dlsym(h, f)	((func_ptr)dl_sym(h, f))
#define pg_dlclose(h)	dl_close(h)
#define	pg_dlerror()	dl_error()

/* port.c */

extern void init_address_fixup(void);

/* strdup.c: strdup() is not part of libc on Ultrix */
extern char* strdup(char const*);

#endif 	/* PORT_PORTOS_H */
