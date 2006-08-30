/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tcop/dest.c,v 1.70 2006/08/30 23:34:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - initialize the destination at start of command
 *		CreateDestReceiver - create tuple receiver object for destination
 *		EndCommand - clean up the destination at end of command
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 */

#include "postgres.h"

#include "access/printtup.h"
#include "access/xact.h"
#include "commands/copy.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/portal.h"


/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static void
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
}

static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
	/* this is used for both shutdown and destroy methods */
}

/* ----------------
 *		static DestReceiver structs for dest types needing no local state
 * ----------------
 */
static DestReceiver donothingDR = {
	donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
	DestNone
};

static DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	DestDebug
};

static DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	DestSPI
};

/* Globally available receiver for DestNone */
DestReceiver *None_Receiver = &donothingDR;


/* ----------------
 *		BeginCommand - initialize the destination at start of command
 * ----------------
 */
void
BeginCommand(const char *commandTag, CommandDest dest)
{
	/* Nothing to do at present */
}

/* ----------------
 *		CreateDestReceiver - return appropriate receiver function set for dest
 *
 * Note: a Portal must be specified for destinations DestRemote,
 * DestRemoteExecute, and DestTuplestore.  It can be NULL for the others.
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest, Portal portal)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			if (portal == NULL)
				elog(ERROR, "no portal specified for DestRemote receiver");
			return printtup_create_DR(dest, portal);

		case DestNone:
			return &donothingDR;

		case DestDebug:
			return &debugtupDR;

		case DestSPI:
			return &spi_printtupDR;

		case DestTuplestore:
			if (portal == NULL)
				elog(ERROR, "no portal specified for DestTuplestore receiver");
			if (portal->holdStore == NULL ||
				portal->holdContext == NULL)
				elog(ERROR, "portal has no holdStore");
			return CreateTuplestoreDestReceiver(portal->holdStore,
												portal->holdContext);

		case DestIntoRel:
			return CreateIntoRelDestReceiver();

		case DestCopyOut:
			return CreateCopyDestReceiver();
	}

	/* should never get here */
	return &donothingDR;
}

/* ----------------
 *		EndCommand - clean up the destination at end of command
 * ----------------
 */
void
EndCommand(const char *commandTag, CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			pq_puttextmessage('C', commandTag);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
			break;
	}
}

/* ----------------
 *		NullCommand - tell dest that an empty query string was recognized
 *
 *		In FE/BE protocol version 1.0, this hack is necessary to support
 *		libpq's crufty way of determining whether a multiple-command
 *		query string is done.  In protocol 2.0 it's probably not really
 *		necessary to distinguish empty queries anymore, but we still do it
 *		for backwards compatibility with 1.0.  In protocol 3.0 it has some
 *		use again, since it ensures that there will be a recognizable end
 *		to the response to an Execute message.
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:

			/*
			 * tell the fe that we saw an empty query string.  In protocols
			 * before 3.0 this has a useless empty-string message body.
			 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
				pq_putemptymessage('I');
			else
				pq_puttextmessage('I', "");
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
			break;
	}
}

/* ----------------
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *		The ReadyForQuery message is sent in protocol versions 2.0 and up
 *		so that the FE can tell when we are done processing a query string.
 *		In versions 3.0 and up, it also carries a transaction state indicator.
 *
 *		Note that by flushing the stdio buffer here, we can avoid doing it
 *		most other places and thus reduce the number of separate packets sent.
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
			{
				StringInfoData buf;

				pq_beginmessage(&buf, 'Z');
				pq_sendbyte(&buf, TransactionBlockStatusCode());
				pq_endmessage(&buf);
			}
			else if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
				pq_putemptymessage('Z');
			/* Flush output at end of cycle in any case. */
			pq_flush();
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
			break;
	}
}
