/*-------------------------------------------------------------------------
 *
 * tuptoaster.h
 *	  POSTGRES definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000, PostgreSQL Development Team
 *
 * $Id: tuptoaster.h,v 1.2 2000/04/12 17:16:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTOASTER_H
#define TUPTOASTER_H

#ifdef TUPLE_TOASTER_ACTIVE

#include "postgres.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupmacs.h"
#include "utils/rel.h"


extern void heap_tuple_toast_attrs(Relation rel,
					   HeapTuple newtup, HeapTuple oldtup);

extern varattrib *heap_tuple_untoast_attr(varattrib * attr);

#endif	 /* TUPLE_TOASTER_ACTIVE */


#endif	 /* TUPTOASTER_H */
