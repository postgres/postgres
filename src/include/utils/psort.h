/*-------------------------------------------------------------------------
 *
 * psort.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: psort.h,v 1.21 1999/07/16 17:07:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSORT_H
#define PSORT_H

#include "access/relscan.h"
#include "nodes/plannodes.h"
#include "storage/fd.h"
#include "utils/lselect.h"

#define MAXTAPES		7		/* See Knuth Fig. 70, p273 */

struct tape
{
	int			tp_dummy;		/* (D) */
	int			tp_fib;			/* (A) */
	BufFile    *tp_file;		/* (TAPE) */
	struct tape *tp_prev;
};

struct cmplist
{
	int			cp_attn;		/* attribute number */
	int			cp_num;			/* comparison function code */
	int			cp_rev;			/* invert comparison flag */
	struct cmplist *cp_next;	/* next in chain */
};

/* This structure preserves the state of psort between calls from different
 * nodes to its interface functions. Basically, it includes all of the global
 * variables in psort. In case you were wondering, pointers to these structures
 * are included in Sort node structures.						-Rex 2.6.1995
 */
typedef struct Psortstate
{
	LeftistContextData treeContext;

	int			TapeRange;
	int			Level;
	int			TotalDummy;
	struct tape Tape[MAXTAPES];

	int			BytesRead;
	int			BytesWritten;
	int			tupcount;

	struct leftist *Tuples;

	BufFile    *psort_grab_file;
	long		psort_current;	/* could be file offset, or array index */
	long		psort_saved;	/* could be file offset, or array index */
	bool		using_tape_files;
	bool		all_fetched;	/* this is for cursors */

	HeapTuple  *memtuples;
} Psortstate;

#ifdef	EBUG
#include "storage/buf.h"
#include "storage/bufmgr.h"

#define PDEBUG(PROC, S1)\
elog(DEBUG, "%s:%d>> PROC: %s.", __FILE__, __LINE__, S1)

#define PDEBUG2(PROC, S1, D1)\
elog(DEBUG, "%s:%d>> PROC: %s %d.", __FILE__, __LINE__, S1, D1)

#define PDEBUG4(PROC, S1, D1, S2, D2)\
elog(DEBUG, "%s:%d>> PROC: %s %d, %s %d.", __FILE__, __LINE__, S1, D1, S2, D2)

#define VDEBUG(VAR, FMT)\
elog(DEBUG, "%s:%d>> VAR =FMT", __FILE__, __LINE__, VAR)

#define ASSERT(EXPR, STR)\
if (!(EXPR)) elog(FATAL, "%s:%d>> %s", __FILE__, __LINE__, STR)

#define TRACE(VAL, CODE)\
if (1) CODE; else

#else
#define PDEBUG(MSG)
#define VDEBUG(VAR, FMT)
#define ASSERT(EXPR, MSG)
#define TRACE(VAL, CODE)
#endif

/* psort.c */
extern bool psort_begin(Sort *node, int nkeys, ScanKey key);
extern HeapTuple psort_grabtuple(Sort *node, bool *should_free);
extern void psort_markpos(Sort *node);
extern void psort_restorepos(Sort *node);
extern void psort_end(Sort *node);
extern void psort_rescan(Sort *node);

#endif	 /* PSORT_H */
