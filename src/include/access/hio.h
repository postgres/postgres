/*-------------------------------------------------------------------------
 *
 * hio.h
 *	  POSTGRES heap access method input/output definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hio.h,v 1.15 2000/07/03 02:54:17 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HIO_H
#define HIO_H

#include "access/htup.h"

extern void RelationPutHeapTuple(Relation relation, Buffer buffer,
					 HeapTuple tuple);
extern Buffer RelationGetBufferForTuple(Relation relation, Size len, Buffer Ubuf);

#endif	 /* HIO_H */
