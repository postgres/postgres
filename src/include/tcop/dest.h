/*-------------------------------------------------------------------------
 *
 * dest.h--
 *    	Whenever the backend is submitted a query, the results
 *	have to go someplace - either to the standard output,
 *	to a local portal buffer or to a remote portal buffer.
 *
 *    -	stdout is the destination only when we are running a
 *	backend without a postmaster and are returning results
 *	back to the user.
 *
 *    -	a local portal buffer is the destination when a backend
 *	executes a user-defined function which calls PQexec() or
 *	PQfn().  In this case, the results are collected into a
 *	PortalBuffer which the user's function may diddle with.
 *
 *    -	a remote portal buffer is the destination when we are
 *	running a backend with a frontend and the frontend executes
 *	PQexec() or PQfn().  In this case, the results are sent
 *	to the frontend via the pq_ functions.
 *
 *    - None is the destination when the system executes
 *	a query internally.  This is not used now but it may be
 *	useful for the parallel optimiser/executor.
 *	
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dest.h,v 1.7 1997/08/27 09:05:09 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEST_H
#define DEST_H

#include <access/tupdesc.h>

/* ----------------
 *	CommandDest is used to allow the results of calling
 *	pg_eval() to go to the right place.
 * ----------------
 */
typedef enum {
    None,		/* results are discarded */
    Debug,		/* results go to debugging output */
    Local,		/* results go in local portal buffer */
    Remote,		/* results sent to frontend process */
    CopyBegin,		/* results sent to frontend process but are strings */
    CopyEnd,	        /* results sent to frontend process but are strings */
    RemoteInternal      /* results sent to frontend process in internal
			   (binary) form */
} CommandDest;


/* AttrInfo* replaced with TupleDesc, now that TupleDesc also has within it
   the number of attributes

typedef struct AttrInfo {
    int			numAttr;
    AttributeTupleForm	*attrs;
} AttrInfo;
*/

extern void (*DestToFunction(CommandDest dest))();
extern void EndCommand(char *commandTag, CommandDest dest);
extern void SendCopyBegin(void);
extern void ReceiveCopyBegin(void);
extern void NullCommand(CommandDest dest);
extern void BeginCommand(char *pname, int operation, TupleDesc attinfo,
			 bool isIntoRel, bool isIntoPortal, char *tag,
			 CommandDest dest);
extern void UpdateCommandInfo (int operation, Oid lastoid, uint32 tuples);

#endif  /* DEST_H */
