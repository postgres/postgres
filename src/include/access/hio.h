/*-------------------------------------------------------------------------
 *
 * hio.h
 *	  POSTGRES heap access method input/output definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hio.h,v 1.18 2001/05/16 22:35:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HIO_H
#define HIO_H

#include "access/htup.h"

extern void RelationPutHeapTuple(Relation relation, Buffer buffer,
					 HeapTuple tuple);
extern Buffer RelationGetBufferForTuple(Relation relation, Size len,
										BlockNumber minblocknum);

#endif	 /* HIO_H */
