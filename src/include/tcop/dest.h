/*-------------------------------------------------------------------------
 *
 * dest.h
 *	  support for communication destinations
 *
 * Whenever the backend executes a query, the results
 * have to go someplace.
 *
 *	  - stdout is the destination only when we are running a
 *		standalone backend (no postmaster) and are returning results
 *		back to an interactive user.
 *
 *	  - a remote process is the destination when we are
 *		running a backend with a frontend and the frontend executes
 *		PQexec() or PQfn().  In this case, the results are sent
 *		to the frontend via the functions in backend/libpq.
 *
 *	  - None is the destination when the system executes
 *		a query internally.  The results are discarded.
 *
 * dest.c defines three functions that implement destination management:
 *
 * BeginCommand: initialize the destination at start of command.
 * DestToFunction: return a pointer to a struct of destination-specific
 * receiver functions.
 * EndCommand: clean up the destination at end of command.
 *
 * BeginCommand/EndCommand are executed once per received SQL query.
 *
 * DestToFunction, and the receiver functions it links to, are executed
 * each time we run the executor to produce tuples, which may occur
 * multiple times per received query (eg, due to additional queries produced
 * by rewrite rules).
 *
 * The DestReceiver object returned by DestToFunction may be a statically
 * allocated object (for destination types that require no local state)
 * or can be a palloc'd object that has DestReceiver as its first field
 * and contains additional fields (see printtup.c for an example).	These
 * additional fields are then accessible to the DestReceiver functions
 * by casting the DestReceiver* pointer passed to them.
 * The palloc'd object is pfree'd by the DestReceiver's cleanup function.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dest.h,v 1.35 2003/05/05 00:44:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEST_H
#define DEST_H

#include "access/htup.h"


/* buffer size to use for command completion tags */
#define COMPLETION_TAG_BUFSIZE	64


/* ----------------
 *		CommandDest is a simplistic means of identifying the desired
 *		destination.  Someday this will probably need to be improved.
 *
 * Note: only the values None, Debug, Remote are legal for the global
 * variable whereToSendOutput.  The other values may be selected
 * as the destination for individual commands.
 * ----------------
 */
typedef enum
{
	None,						/* results are discarded */
	Debug,						/* results go to debugging output */
	Remote,						/* results sent to frontend process */
	RemoteInternal,				/* results sent to frontend process in
								 * internal (binary) form */
	SPI,						/* results sent to SPI manager */
	Tuplestore,					/* results sent to Tuplestore */
	RemoteExecute,				/* sent to frontend, in Execute command */
	RemoteExecuteInternal		/* same, but binary format */
} CommandDest;

/* ----------------
 *		DestReceiver is a base type for destination-specific local state.
 *		In the simplest cases, there is no state info, just the function
 *		pointers that the executor must call.
 * ----------------
 */
typedef struct _DestReceiver DestReceiver;

struct _DestReceiver
{
	/* Called for each tuple to be output: */
	void		(*receiveTuple) (HeapTuple tuple, TupleDesc typeinfo,
											 DestReceiver *self);
	/* Initialization and teardown: */
	void		(*setup) (DestReceiver *self, int operation,
							 const char *portalName, TupleDesc typeinfo);
	void		(*cleanup) (DestReceiver *self);
	/* Private fields might appear beyond this point... */
};

/* The primary destination management functions */

extern void BeginCommand(const char *commandTag, CommandDest dest);
extern DestReceiver *DestToFunction(CommandDest dest);
extern void EndCommand(const char *commandTag, CommandDest dest);

/* Additional functions that go with destination management, more or less. */

extern void NullCommand(CommandDest dest);
extern void ReadyForQuery(CommandDest dest);

#endif   /* DEST_H */
