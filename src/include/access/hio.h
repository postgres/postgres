/*-------------------------------------------------------------------------
 *
 * hio.h
 *	  POSTGRES heap access method input/output definitions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/hio.h,v 1.30 2006/03/05 15:58:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HIO_H
#define HIO_H

#include "access/htup.h"

extern void RelationPutHeapTuple(Relation relation, Buffer buffer,
					 HeapTuple tuple);
extern Buffer RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, bool use_fsm);

#endif   /* HIO_H */
