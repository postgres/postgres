/*-------------------------------------------------------------------------
 *
 * execdebug.h--
 *	  #defines governing debugging behaviour in the executor
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execdebug.h,v 1.6 1998/09/01 04:35:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECDEBUG_H
#define EXECDEBUG_H

#include "access/printtup.h"

/* ----------------------------------------------------------------
 *		debugging defines.
 *
 *		If you want certain debugging behaviour, then #define
 *		the variable to 1, else #undef it. -cim 10/26/89
 * ----------------------------------------------------------------
 */

/* ----------------
 *		EXEC_DEBUGSTORETUP is for tuple table debugging - this
 *		will print a message every time we call ExecStoreTuple.
 *		-cim 3/20/91
 * ----------------
 */
#undef EXEC_DEBUGSTORETUP

/* ----------------
 *		EXEC_TUPLECOUNT is a #define which causes the
 *		executor keep track of tuple counts.  This might be
 *		causing some problems with the decstation stuff so
 *		you might want to undefine this if you are doing work
 *		on the decs  - cim 10/20/89
 * ----------------
 */
#undef EXEC_TUPLECOUNT

/* ----------------
 *		EXEC_SHOWBUFSTATS controls whether or not buffer statistics
 *		are shown for each query.  -cim 2/9/89
 * ----------------
 */
#undef EXEC_SHOWBUFSTATS

/* ----------------
 *		EXEC_CONTEXTDEBUG turns on the printing of debugging information
 *		by CXT_printf() calls regarding which memory context is the
 *		CurrentMemoryContext for palloc() calls.
 * ----------------
 */
#undef EXEC_CONTEXTDEBUG

/* ----------------
 *		EXEC_RETURNSIZE is a compile flag governing the
 *		behaviour of lispFmgr..  See ExecMakeFunctionResult().
 *		Undefining this avoids a problem in the system cache.
 *
 *		Note: undefining this means that there is incorrect
 *			  information in the const nodes corresponding
 *			  to function (or operator) results.  The thing is,
 *			  99% of the time this is fine because when you do
 *			  something like x = emp.sal + 1, you already know
 *			  the type and size of x so the fact that + didn't
 *			  return the correct size doesn't matter.
 *			  With variable length stuff the size is stored in
 *			  the first few bytes of the data so again, it's
 *			  not likely to matter.
 * ----------------
 */
#undef EXEC_RETURNSIZE

/* ----------------
 *		EXEC_UTILSDEBUG is a flag which turns on debugging of the
 *		executor utilities by EU_printf() in eutils.c
 * ----------------
 */
#undef EXEC_UTILSDEBUG

/* ----------------
 *		EXEC_NESTLOOPDEBUG is a flag which turns on debugging of the
 *		nest loop node by NL_printf() and ENL_printf() in nestloop.c
 * ----------------
 */
#undef EXEC_NESTLOOPDEBUG

/* ----------------
 *		EXEC_PROCDEBUG is a flag which turns on debugging of
 *		ExecProcNode() by PN_printf() in procnode.c
 * ----------------
 */
#undef EXEC_PROCDEBUG

/* ----------------
 *		EXEC_EVALDEBUG is a flag which turns on debugging of
 *		ExecEval and ExecTargetList() stuff by EV_printf() in qual.c
 * ----------------
 */
#undef EXEC_EVALDEBUG

/* ----------------
 *		EXEC_SCANDEBUG is a flag which turns on debugging of
 *		the ExecSeqScan() stuff by S_printf() in seqscan.c
 * ----------------
 */
#undef EXEC_SCANDEBUG

/* ----------------
 *		EXEC_SORTDEBUG is a flag which turns on debugging of
 *		the ExecSort() stuff by SO_printf() in sort.c
 * ----------------
 */
#undef EXEC_SORTDEBUG

/* ----------------
 *		EXEC_MERGEJOINDEBUG is a flag which turns on debugging of
 *		the ExecMergeJoin() stuff by MJ_printf() in mergejoin.c
 * ----------------
 */
#undef EXEC_MERGEJOINDEBUG

/* ----------------
 *		EXEC_MERGEJOINPFREE is a flag which causes merge joins
 *		to pfree intermittant tuples (which is the proper thing)
 *		Not defining this means we avoid menory management problems
 *		at the cost of doing deallocation of stuff only at the
 *		end of the transaction
 * ----------------
 */
#undef EXEC_MERGEJOINPFREE

/* ----------------
 *		EXEC_DEBUGINTERACTIVE is a flag which enables the
 *		user to issue "DEBUG" commands from an interactive
 *		backend.
 * ----------------
 */
#undef EXEC_DEBUGINTERACTIVE

/* ----------------
 *		EXEC_DEBUGVARIABLEFILE is string, which if defined will
 *		be loaded when the executor is initialized.  If this
 *		string is not defined then nothing will be loaded..
 *
 *		Example:
 *
 * #define EXEC_DEBUGVARIABLEFILE "/a/postgres/cimarron/.pg_debugvars"
 #
 *		Note: since these variables are read at execution time,
 *		they can't affect the first query.. this hack should be
 *		replaced by something better sometime. -cim 11/2/89
 * ----------------
 */
#undef EXEC_DEBUGVARIABLEFILE

/* ----------------------------------------------------------------
 *		#defines controlled by above definitions
 *
 *		Note: most of these are "incomplete" because I didn't
 *			  need the ones not defined.  More should be added
 *			  only as necessary -cim 10/26/89
 * ----------------------------------------------------------------
 */
#define T_OR_F(b)				(b ? "true" : "false")
#define NULL_OR_TUPLE(slot)		(TupIsNull(slot) ? "null" : "a tuple")


/* #define EXEC_TUPLECOUNT - XXX take out for now for executor stubbing -- jolly*/
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
#endif	 /* EXEC_TUPLECOUNT */

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
#endif	 /* EXEC_CONTEXTDEBUG */

/* ----------------
 *		eutils debugging defines
 * ----------------
 */
#ifdef EXEC_UTILSDEBUG
#define EU_nodeDisplay(l)				nodeDisplay(l, 0)
#define EU_printf(s)					printf(s)
#define EU1_printf(s, a)				printf(s, a)
#define EU2_printf(s, a)				printf(s, a, b)
#define EU3_printf(s, a)				printf(s, a, b, c)
#define EU4_printf(s, a, b, c, d)		printf(s, a, b, c, d)
#else
#define EU_nodeDisplay(l)
#define EU_printf(s)
#define EU1_printf(s, a)
#define EU2_printf(s, a, b)
#define EU3_printf(s, a, b, c)
#define EU4_printf(s, a, b, c, d)
#endif	 /* EXEC_UTILSDEBUG */


/* ----------------
 *		nest loop debugging defines
 * ----------------
 */
#ifdef EXEC_NESTLOOPDEBUG
#define NL_nodeDisplay(l)				nodeDisplay(l, 0)
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
#endif	 /* EXEC_NESTLOOPDEBUG */

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
#endif	 /* EXEC_PROCDEBUG */

/* ----------------
 *		exec eval / target list debugging defines
 * ----------------
 */
#ifdef EXEC_EVALDEBUG
#define EV_nodeDisplay(l)				nodeDisplay(l, 0)
#define EV_printf(s)					printf(s)
#define EV1_printf(s, a)				printf(s, a)
#define EV5_printf(s, a, b, c, d, e)	printf(s, a, b, c, d, e)
#else
#define EV_nodeDisplay(l)
#define EV_printf(s)
#define EV1_printf(s, a)
#define EV5_printf(s, a, b, c, d, e)
#endif	 /* EXEC_EVALDEBUG */

/* ----------------
 *		scan debugging defines
 * ----------------
 */
#ifdef EXEC_SCANDEBUG
#define S_nodeDisplay(l)				nodeDisplay(l, 0)
#define S_printf(s)						printf(s)
#define S1_printf(s, p)					printf(s, p)
#else
#define S_nodeDisplay(l)
#define S_printf(s)
#define S1_printf(s, p)
#endif	 /* EXEC_SCANDEBUG */

/* ----------------
 *		sort node debugging defines
 * ----------------
 */
#ifdef EXEC_SORTDEBUG
#define SO_nodeDisplay(l)				nodeDisplay(l, 0)
#define SO_printf(s)					printf(s)
#define SO1_printf(s, p)				printf(s, p)
#else
#define SO_nodeDisplay(l)
#define SO_printf(s)
#define SO1_printf(s, p)
#endif	 /* EXEC_SORTDEBUG */

/* ----------------
 *		merge join debugging defines
 * ----------------
 */
#ifdef EXEC_MERGEJOINDEBUG
#define MJ_nodeDisplay(l)				nodeDisplay(l, 0)
#define MJ_printf(s)					printf(s)
#define MJ1_printf(s, p)				printf(s, p)
#define MJ2_printf(s, p1, p2)			printf(s, p1, p2)
#define MJ_debugtup(tuple, type)		debugtup(tuple, type)
#define MJ_dump(context, state)			ExecMergeTupleDump(econtext, state)
#define MJ_DEBUG_QUAL(clause, res) \
  MJ2_printf("  ExecQual(%s, econtext) returns %s\n", \
			 CppAsString(clause), T_OR_F(res));

#define MJ_DEBUG_MERGE_COMPARE(qual, res) \
  MJ2_printf("  MergeCompare(mergeclauses, %s, ..) returns %s\n", \
			 CppAsString(qual), T_OR_F(res));

#define MJ_DEBUG_PROC_NODE(slot) \
  MJ2_printf("  %s = ExecProcNode(innerPlan) returns %s\n", \
			 CppAsString(slot), NULL_OR_TUPLE(slot));
#else
#define MJ_nodeDisplay(l)
#define MJ_printf(s)
#define MJ1_printf(s, p)
#define MJ2_printf(s, p1, p2)
#define MJ_debugtup(tuple, type)
#define MJ_dump(context, state)
#define MJ_DEBUG_QUAL(clause, res)
#define MJ_DEBUG_MERGE_COMPARE(qual, res)
#define MJ_DEBUG_PROC_NODE(slot)
#endif	 /* EXEC_MERGEJOINDEBUG */

/* ----------------------------------------------------------------
 *		DO NOT DEFINE THESE EVER OR YOU WILL BURN!
 * ----------------------------------------------------------------
 */
/* ----------------
 *		DOESNOTWORK is currently placed around memory manager
 *		code that is known to cause problems.  Code in between
 *		is likely not converted and probably won't work anyways.
 * ----------------
 */
#undef DOESNOTWORK

/* ----------------
 *		PERHAPSNEVER is placed around the "scan attribute"
 *		support code for the rule manager because for now we
 *		do things inefficiently.  The correct solution to our
 *		problem is to add code to the parser/planner to save
 *		attribute information for the rule manager rather than
 *		have the executor have to grope through the entire plan
 *		for it so if we ever decide to make things better,
 *		we should probably delete the stuff in between PERHAPSNEVER..
 * ----------------
 */
#undef PERHAPSNEVER

/* ----------------
 *		NOTYET is placed around any code not yet implemented
 *		in the executor.  Only remove these when actually implementing
 *		said code.
 * ----------------
 */
#undef NOTYET

extern long NDirectFileRead;
extern long NDirectFileWrite;

#endif	 /* ExecDebugIncluded */
