/*-------------------------------------------------------------------------
 *
 * tuplestore.h
 *	  Generalized routines for temporary tuple storage.
 *
 * This module handles temporary storage of tuples for purposes such
 * as Materialize nodes, hashjoin batch files, etc.  It is essentially
 * a dumbed-down version of tuplesort.c; it does no sorting of tuples
 * but can only store and regurgitate a sequence of tuples.  However,
 * because no sort is required, it is allowed to start reading the sequence
 * before it has all been written.	This is particularly useful for cursors,
 * because it allows random access within the already-scanned portion of
 * a query without having to process the underlying scan to completion.
 * A temporary file is used to handle the data if it exceeds the
 * space limit specified by the caller.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tuplestore.h,v 1.13 2003/08/04 02:40:15 momjian Exp $
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
					  bool interXact,
					  int maxKBytes);

extern void tuplestore_puttuple(Tuplestorestate *state, void *tuple);

/* tuplestore_donestoring() used to be required, but is no longer used */
#define tuplestore_donestoring(state)	((void) 0)

/* backwards scan is only allowed if randomAccess was specified 'true' */
extern void *tuplestore_gettuple(Tuplestorestate *state, bool forward,
					bool *should_free);

#define tuplestore_getheaptuple(state, forward, should_free) \
	((HeapTuple) tuplestore_gettuple(state, forward, should_free))

extern void tuplestore_end(Tuplestorestate *state);

extern bool tuplestore_ateof(Tuplestorestate *state);

extern void tuplestore_rescan(Tuplestorestate *state);
extern void tuplestore_markpos(Tuplestorestate *state);
extern void tuplestore_restorepos(Tuplestorestate *state);

#endif   /* TUPLESTORE_H */
