/*-------------------------------------------------------------------------
 *
 * printtup.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: printtup.h,v 1.7 1999/01/27 00:36:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRINTTUP_H
#define PRINTTUP_H

#include <tcop/dest.h>

extern DestReceiver* printtup_create_DR(void);
extern void showatts(char *name, TupleDesc attinfo);
extern void debugtup(HeapTuple tuple, TupleDesc typeinfo,
					 DestReceiver* self);
extern void printtup_internal(HeapTuple tuple, TupleDesc typeinfo,
							  DestReceiver* self);

/* XXX this one is really in executor/spi.c */
extern void spi_printtup(HeapTuple tuple, TupleDesc tupdesc,
						 DestReceiver* self);

extern int	getTypeOutAndElem(Oid type, Oid* typOutput, Oid* typElem);

#endif	 /* PRINTTUP_H */
