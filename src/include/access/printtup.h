/*-------------------------------------------------------------------------
 *
 * printtup.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: printtup.h,v 1.26 2003/05/06 20:26:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRINTTUP_H
#define PRINTTUP_H

#include "tcop/dest.h"

extern DestReceiver *printtup_create_DR(CommandDest dest);

extern void SendRowDescriptionMessage(TupleDesc typeinfo, List *targetlist);

extern void debugStartup(DestReceiver *self, int operation,
		   const char *portalName, TupleDesc typeinfo, List *targetlist);
extern void debugtup(HeapTuple tuple, TupleDesc typeinfo,
		 DestReceiver *self);

/* XXX these are really in executor/spi.c */
extern void spi_dest_startup(DestReceiver *self, int operation,
		   const char *portalName, TupleDesc typeinfo, List *targetlist);
extern void spi_printtup(HeapTuple tuple, TupleDesc typeinfo,
			 DestReceiver *self);

#endif   /* PRINTTUP_H */
