/*-------------------------------------------------------------------------
 *
 * libpq-be.h--
 *    This file contains definitions for structures and
 *    externs for functions used by the POSTGRES backend.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-be.h,v 1.1 1996/08/28 07:22:59 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

/* ----------------
 *	include stuff common to fe and be
 * ----------------
 */
#include "libpq/libpq.h"
#include "access/htup.h"

#include "access/tupdesc.h"

/* ----------------
 *	declarations for backend libpq support routines
 * ----------------
 */

/* in be-dumpdata.c */
extern void be_portalinit(void);
extern void be_portalpush(PortalEntry *entry);
extern PortalEntry *be_portalpop(void);
extern PortalEntry *be_currentportal();
extern PortalEntry *be_newportal(void);
extern void be_typeinit(PortalEntry *entry, TupleDesc attrs,
			int natts);
extern void be_printtup(HeapTuple tuple, TupleDesc typeinfo);


/* in be-pqexec.c */
extern char *PQfn(int fnid, int *result_buf, int result_len, int result_is_int, 
		  PQArgBlock *args, int nargs);
extern char *PQexec(char *query);
extern int pqtest_PQexec(char *q);
extern char *strmake(char *str, int len);
extern int pqtest_PQfn(char *q);
extern int32 pqtest(struct varlena *vlena);

#endif /* LIBPQ_BE_H */
