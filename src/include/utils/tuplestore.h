/*-------------------------------------------------------------------------
 *
 * tuplestore.h
 *	  Generalized routines for temporary tuple storage.
 *
 * This module handles temporary storage of tuples for purposes such
 * as Materialize nodes, hashjoin batch files, etc.  It is essentially
 * a dumbed-down version of tuplesort.c; it does no sorting of tuples
 * but can only store a sequence of tuples and regurgitate it later.
 * A temporary file is used to handle the data if it exceeds the
 * space limit specified by the caller.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tuplestore.h,v 1.2 2001/01/24 19:43:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESTORE_H
#define TUPLESTORE_H

#include "access/htup.h"

/* Tuplestorestate is an opaque type whose details are not known outside
 * tuplestore.c.
 */
typedef struct Tuplestorestate Tuplestorestate;

/*
 * Currently we only need to store HeapTuples, but it would be easy
 * to support the same behavior for IndexTuples and/or bare Datums.
 */

extern Tuplestorestate *tuplestore_begin_heap(bool randomAccess,
											  int maxKBytes);

extern void tuplestore_puttuple(Tuplestorestate *state, void *tuple);

extern void tuplestore_donestoring(Tuplestorestate *state);

extern void *tuplestore_gettuple(Tuplestorestate *state, bool forward,
								 bool *should_free);

#define tuplestore_getheaptuple(state, forward, should_free) \
	((HeapTuple) tuplestore_gettuple(state, forward, should_free))

extern void tuplestore_end(Tuplestorestate *state);

/*
 * These routines may only be called if randomAccess was specified 'true'.
 * Likewise, backwards scan in gettuple/getdatum is only allowed if
 * randomAccess was specified.
 */

extern void tuplestore_rescan(Tuplestorestate *state);
extern void tuplestore_markpos(Tuplestorestate *state);
extern void tuplestore_restorepos(Tuplestorestate *state);

#endif	 /* TUPLESTORE_H */
