/*-------------------------------------------------------------------------
 *
 * execdebug.h
 *	  #defines governing debugging behaviour in the executor
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execdebug.h,v 1.21 2003/08/04 02:40:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDEBUG_H
#define EXECDEBUG_H

#include "executor/executor.h"
#include "nodes/print.h"

/* ----------------------------------------------------------------
 *		debugging defines.
 *
 *		If you want certain debugging behaviour, then #define
 *		the variable to 1. No need to explicitly #undef by default,
 *		since we can use -D compiler options to enable features.
 *		- thomas 1999-02-20
 * ----------------------------------------------------------------
 */

/* ----------------
 *		EXEC_TUPLECOUNT is a #define which causes the
 *		executor keep track of tuple counts.  This might be
 *		causing some problems with the decstation stuff so
 *		you might want to undefine this if you are doing work
 *		on the decs  - cim 10/20/89
 * ----------------
#undef EXEC_TUPLECOUNT
 */

/* ----------------
 *		EXEC_CONTEXTDEBUG turns on the printing of debugging information
 *		by CXT_printf() calls regarding which memory context is the
 *		CurrentMemoryContext for palloc() calls.
 * ----------------
#undef EXEC_CONTEXTDEBUG
 */

/* ----------------
 *		EXEC_UTILSDEBUG is a flag which turns on debugging of the
 *		executor utilities by EU_printf() in eutils.c
 * ----------------
#undef EXEC_UTILSDEBUG
 */

/* ----------------
 *		EXEC_NESTLOOPDEBUG is a flag which turns on debugging of the
 *		nest loop node by NL_printf() and ENL_printf() in nestloop.c
 * ----------------
#undef EXEC_NESTLOOPDEBUG
 */

/* ----------------
 *		EXEC_PROCDEBUG is a flag which turns on debugging of
 *		ExecProcNode() by PN_printf() in procnode.c
 * ----------------
#undef EXEC_PROCDEBUG
 */

/* ----------------
 *		EXEC_EVALDEBUG is a flag which turns on debugging of
 *		ExecEval and ExecTargetList() stuff by EV_printf() in qual.c
 * ----------------
#undef EXEC_EVALDEBUG
 */

/* ----------------
 *		EXEC_SCANDEBUG is a flag which turns on debugging of
 *		the ExecSeqScan() stuff by S_printf() in seqscan.c
 * ----------------
#undef EXEC_SCANDEBUG
 */

/* ----------------
 *		EXEC_SORTDEBUG is a flag which turns on debugging of
 *		the ExecSort() stuff by SO_printf() in sort.c
 * ----------------
#undef EXEC_SORTDEBUG
 */

/* ----------------
 *		EXEC_MERGEJOINDEBUG is a flag which turns on debugging of
 *		the ExecMergeJoin() stuff by MJ_printf() in mergejoin.c
 * ----------------
#undef EXEC_MERGEJOINDEBUG
 */

/* ----------------------------------------------------------------
 *		#defines controlled by above definitions
 *
 *		Note: most of these are "incomplete" because I didn't
 *			  need the ones not defined.  More should be added
 *			  only as necessary -cim 10/26/89
 * ----------------------------------------------------------------
 */
#define T_OR_F(b)				((b) ? "true" : "false")
#define NULL_OR_TUPLE(slot)		(TupIsNull(slot) ? "null" : "a tuple")


/* ----------------
 *		tuple count debugging defines
 * ----------------
 */
#ifdef EXEC_TUPLECOUNT
extern int	NTupleProcessed;
extern int	NTupleRetrieved;
extern int	NTupleReplaced;
extern int	NTupleAppended;
extern int	NTupleDeleted;
extern int	NIndexTupleProcessed;
extern int	NIndexTupleInserted;

#define IncrRetrieved()			NTupleRetrieved++
#define IncrAppended()			NTupleAppended++
#define IncrDeleted()			NTupleDeleted++
#define IncrReplaced()			NTupleReplaced++
#define IncrInserted()			NTupleInserted++
#define IncrProcessed()			NTupleProcessed++
#define IncrIndexProcessed()	NIndexTupleProcessed++
#define IncrIndexInserted()		NIndexTupleInserted++
#else
/*								stop compiler warnings */
#define IncrRetrieved()			(void)(0)
#define IncrAppended()			(void)(0)
#define IncrDeleted()			(void)(0)
#define IncrReplaced()			(void)(0)
#define IncrInserted()			(void)(0)
#define IncrProcessed()			(void)(0)
#define IncrIndexProcessed()	(void)(0)
#define IncrIndexInserted()		(void)(0)
#endif   /* EXEC_TUPLECOUNT */

/* ----------------
 *		memory context debugging defines
 * ----------------
 */
#ifdef EXEC_CONTEXTDEBUG
#define CXT_printf(s)					printf(s)
#define CXT1_printf(s, a)				printf(s, a)
#else
#define CXT_printf(s)
#define CXT1_printf(s, a)
#endif   /* EXEC_CONTEXTDEBUG */

/* ----------------
 *		eutils debugging defines
 * ----------------
 */
#ifdef EXEC_UTILSDEBUG
#define EU_nodeDisplay(l)				nodeDisplay(l)
#define EU_printf(s)					printf(s)
#define EU1_printf(s, a)				printf(s, a)
#define EU2_printf(s, a, b)				printf(s, a, b)
#define EU3_printf(s, a, b, c)			printf(s, a, b, c)
#define EU4_printf(s, a, b, c, d)		printf(s, a, b, c, d)
#else
#define EU_nodeDisplay(l)
#define EU_printf(s)
#define EU1_printf(s, a)
#define EU2_printf(s, a, b)
#define EU3_printf(s, a, b, c)
#define EU4_printf(s, a, b, c, d)
#endif   /* EXEC_UTILSDEBUG */


/* ----------------
 *		nest loop debugging defines
 * ----------------
 */
#ifdef EXEC_NESTLOOPDEBUG
#define NL_nodeDisplay(l)				nodeDisplay(l)
#define NL_printf(s)					printf(s)
#define NL1_printf(s, a)				printf(s, a)
#define NL4_printf(s, a, b, c, d)		printf(s, a, b, c, d)
#define ENL1_printf(message)			printf("ExecNestLoop: %s\n", message)
#else
#define NL_nodeDisplay(l)
#define NL_printf(s)
#define NL1_printf(s, a)
#define NL4_printf(s, a, b, c, d)
#define ENL1_printf(message)
#endif   /* EXEC_NESTLOOPDEBUG */

/* ----------------
 *		proc node debugging defines
 * ----------------
 */
#ifdef EXEC_PROCDEBUG
#define PN_printf(s)					printf(s)
#define PN1_printf(s, p)				printf(s, p)
#else
#define PN_printf(s)
#define PN1_printf(s, p)
#endif   /* EXEC_PROCDEBUG */

/* ----------------
 *		exec eval / target list debugging defines
 * ----------------
 */
#ifdef EXEC_EVALDEBUG
#define EV_nodeDisplay(l)				nodeDisplay(l)
#define EV_printf(s)					printf(s)
#define EV1_printf(s, a)				printf(s, a)
#define EV5_printf(s, a, b, c, d, e)	printf(s, a, b, c, d, e)
#else
#define EV_nodeDisplay(l)
#define EV_printf(s)
#define EV1_printf(s, a)
#define EV5_printf(s, a, b, c, d, e)
#endif   /* EXEC_EVALDEBUG */

/* ----------------
 *		scan debugging defines
 * ----------------
 */
#ifdef EXEC_SCANDEBUG
#define S_nodeDisplay(l)				nodeDisplay(l)
#define S_printf(s)						printf(s)
#define S1_printf(s, p)					printf(s, p)
#else
#define S_nodeDisplay(l)
#define S_printf(s)
#define S1_printf(s, p)
#endif   /* EXEC_SCANDEBUG */

/* ----------------
 *		sort node debugging defines
 * ----------------
 */
#ifdef EXEC_SORTDEBUG
#define SO_nodeDisplay(l)				nodeDisplay(l)
#define SO_printf(s)					printf(s)
#define SO1_printf(s, p)				printf(s, p)
#else
#define SO_nodeDisplay(l)
#define SO_printf(s)
#define SO1_printf(s, p)
#endif   /* EXEC_SORTDEBUG */

/* ----------------
 *		merge join debugging defines
 * ----------------
 */
#ifdef EXEC_MERGEJOINDEBUG

#define MJ_nodeDisplay(l)				nodeDisplay(l)
#define MJ_printf(s)					printf(s)
#define MJ1_printf(s, p)				printf(s, p)
#define MJ2_printf(s, p1, p2)			printf(s, p1, p2)
#define MJ_debugtup(tuple, type)		debugtup(tuple, type, NULL)
#define MJ_dump(state)					ExecMergeTupleDump(state)
#define MJ_DEBUG_QUAL(clause, res) \
  MJ2_printf("  ExecQual(%s, econtext) returns %s\n", \
			 CppAsString(clause), T_OR_F(res));

#define MJ_DEBUG_MERGE_COMPARE(qual, res) \
  MJ2_printf("  MergeCompare(mergeclauses, %s, ...) returns %s\n", \
			 CppAsString(qual), T_OR_F(res));

#define MJ_DEBUG_PROC_NODE(slot) \
  MJ2_printf("  %s = ExecProcNode(...) returns %s\n", \
			 CppAsString(slot), NULL_OR_TUPLE(slot));

#else

#define MJ_nodeDisplay(l)
#define MJ_printf(s)
#define MJ1_printf(s, p)
#define MJ2_printf(s, p1, p2)
#define MJ_debugtup(tuple, type)
#define MJ_dump(state)
#define MJ_DEBUG_QUAL(clause, res)
#define MJ_DEBUG_MERGE_COMPARE(qual, res)
#define MJ_DEBUG_PROC_NODE(slot)
#endif   /* EXEC_MERGEJOINDEBUG */

/* ----------------------------------------------------------------
 *		DO NOT DEFINE THESE EVER OR YOU WILL BURN!
 * ----------------------------------------------------------------
 */
/* ----------------
 *		NOTYET is placed around any code not yet implemented
 *		in the executor.  Only remove these when actually implementing
 *		said code.
 * ----------------
 */
#undef NOTYET

extern long NDirectFileRead;
extern long NDirectFileWrite;

#endif   /* ExecDebugIncluded */
