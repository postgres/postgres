/*-------------------------------------------------------------------------
 *
 * tuptoaster.c
 *	  Support routines for external and compressed storage of
 *	  variable size attributes.
 *
 * Copyright (c) 2000, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/heap/tuptoaster.c,v 1.4 2000/05/30 00:49:39 momjian Exp $
 *
 *
 * INTERFACE ROUTINES
 *		heap_tuple_toast_attrs -
 *			Try to make a given tuple fit into one page by compressing
 *			or moving off attributes
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"



#ifdef TUPLE_TOASTER_ACTIVE

void
heap_tuple_toast_attrs(Relation rel, HeapTuple newtup, HeapTuple oldtup)
{
	return;
}


varattrib  *
heap_tuple_untoast_attr(varattrib * attr)
{
	elog(ERROR, "heap_tuple_untoast_attr() called");
}


#endif	 /* TUPLE_TOASTER_ACTIVE */
