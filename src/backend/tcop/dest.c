/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/dest.c,v 1.49.2.1 2003/01/21 22:06:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - initialize the destination at start of command
 *		DestToFunction - identify per-tuple processing routines
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
#include "libpq/libpq.h"
#include "libpq/pqformat.h"


/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static void
donothingReceive(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
}

static void
donothingSetup(DestReceiver *self, int operation,
			   const char *portalName, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
}

/* ----------------
 *		static DestReceiver structs for dest types needing no local state
 * ----------------
 */
static DestReceiver donothingDR = {
	donothingReceive, donothingSetup, donothingCleanup
};
static DestReceiver debugtupDR = {
	debugtup, debugSetup, donothingCleanup
};
static DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_setup, donothingCleanup
};

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
 *		DestToFunction - return appropriate receiver function set for dest
 * ----------------
 */
DestReceiver *
DestToFunction(CommandDest dest)
{
	switch (dest)
	{
		case Remote:
			return printtup_create_DR(false);

		case RemoteInternal:
			return printtup_create_DR(true);

		case Debug:
			return &debugtupDR;

		case SPI:
			return &spi_printtupDR;

		case None:
			return &donothingDR;
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
		case RemoteInternal:
			pq_puttextmessage('C', commandTag);
			break;

		case None:
		case Debug:
		case SPI:
			break;
	}
}

/*
 * These are necessary to sync communications between fe/be processes doing
 * COPY rel TO stdout
 *
 * or
 *
 * COPY rel FROM stdin
 *
 * NOTE: the message code letters are changed at protocol version 2.0
 * to eliminate possible confusion with data tuple messages.
 */
void
SendCopyBegin(void)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
		pq_putbytes("H", 1);	/* new way */
	else
		pq_putbytes("B", 1);	/* old way */
}

void
ReceiveCopyBegin(void)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
		pq_putbytes("G", 1);	/* new way */
	else
		pq_putbytes("D", 1);	/* old way */
	/* We *must* flush here to ensure FE knows it can send. */
	pq_flush();
}

/* ----------------
 *		NullCommand - tell dest that an empty query string was recognized
 *
 *		In FE/BE protocol version 1.0, this hack is necessary to support
 *		libpq's crufty way of determining whether a multiple-command
 *		query string is done.  In protocol 2.0 it's probably not really
 *		necessary to distinguish empty queries anymore, but we still do it
 *		for backwards compatibility with 1.0.
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case RemoteInternal:
		case Remote:

			/*
			 * tell the fe that we saw an empty query string
			 */
			pq_putbytes("I", 2);	/* note we send I and \0 */
			break;

		case Debug:
		case None:
		default:
			break;
	}
}

/* ----------------
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *		The ReadyForQuery message is sent in protocol versions 2.0 and up
 *		so that the FE can tell when we are done processing a query string.
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
		case RemoteInternal:
		case Remote:
			if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
				pq_putbytes("Z", 1);
			/* Flush output at end of cycle in any case. */
			pq_flush();
			break;

		case Debug:
		case None:
		default:
			break;
	}
}
