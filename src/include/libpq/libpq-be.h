/*-------------------------------------------------------------------------
 *
 * libpq-be.h--
 *	  This file contains definitions for structures and
 *	  externs for functions used by the POSTGRES backend.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-be.h,v 1.8 1998/01/24 22:49:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

#include <access/htup.h>
#include <access/tupdesc.h>
#include <libpq/libpq.h>

/* ----------------
 *		include stuff common to fe and be
 * ----------------
 */


/* ----------------
 *		declarations for backend libpq support routines
 * ----------------
 */

/* in be-dumpdata.c */
extern void be_portalinit(void);
extern void be_portalpush(PortalEntry *entry);
extern PortalEntry *be_portalpop(void);
extern PortalEntry *be_currentportal(void);
extern PortalEntry *be_newportal(void);
extern void be_typeinit(PortalEntry *entry, TupleDesc attrs,
			int natts);
extern void be_printtup(HeapTuple tuple, TupleDesc typeinfo);


/* in be-pqexec.c */
extern char * PQfn(int fnid, int *result_buf, int result_len, int result_is_int,
	 PQArgBlock *args, int nargs);
extern char *PQexec(char *query);
extern int	pqtest_PQexec(char *q);
extern int	pqtest_PQfn(char *q);
extern int32 pqtest(struct varlena * vlena);

#endif							/* LIBPQ_BE_H */
