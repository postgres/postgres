/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/dest.c,v 1.61 2003/08/04 02:40:04 momjian Exp $
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
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/portal.h"


/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static void
donothingReceive(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
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
	None
};

static DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	Debug
};

static DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	SPI
};

/* Globally available receiver for None */
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
 * Note: a Portal must be specified for destinations Remote, RemoteExecute,
 * and Tuplestore.	It can be NULL for the others.
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest, Portal portal)
{
	switch (dest)
	{
		case Remote:
		case RemoteExecute:
			if (portal == NULL)
				elog(ERROR, "no portal specified for Remote receiver");
			return printtup_create_DR(dest, portal);

		case None:
			return &donothingDR;

		case Debug:
			return &debugtupDR;

		case SPI:
			return &spi_printtupDR;

		case Tuplestore:
			if (portal == NULL)
				elog(ERROR, "no portal specified for Tuplestore receiver");
			if (portal->holdStore == NULL ||
				portal->holdContext == NULL)
				elog(ERROR, "portal has no holdStore");
			return CreateTuplestoreDestReceiver(portal->holdStore,
												portal->holdContext);
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
		case Remote:
		case RemoteExecute:
			pq_puttextmessage('C', commandTag);
			break;

		case None:
		case Debug:
		case SPI:
		case Tuplestore:
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
		case Remote:
		case RemoteExecute:

			/*
			 * tell the fe that we saw an empty query string.  In
			 * protocols before 3.0 this has a useless empty-string
			 * message body.
			 */
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
				pq_putemptymessage('I');
			else
				pq_puttextmessage('I', "");
			break;

		case None:
		case Debug:
		case SPI:
		case Tuplestore:
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
		case Remote:
		case RemoteExecute:
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

		case None:
		case Debug:
		case SPI:
		case Tuplestore:
			break;
	}
}
