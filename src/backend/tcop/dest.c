/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for various communication destinations - see include/tcop/dest.h
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/dest.c,v 1.43 2001/03/22 03:59:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - prepare destination for tuples of the given type
 *		DestToFunction - identify per-tuple processing routines
 *		EndCommand - tell destination that no more tuples will arrive
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 *
 *		There is a second level of initialization/cleanup performed by the
 *		setup/cleanup routines identified by DestToFunction.  This could
 *		probably be merged with the work done by BeginCommand/EndCommand,
 *		but as of right now BeginCommand/EndCommand are used in a rather
 *		unstructured way --- some places call Begin without End, some vice
 *		versa --- so I think I'll just leave 'em alone for now.  tgl 1/99.
 *
 */

#include "postgres.h"

#include "access/printtup.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"


static char CommandInfo[32] = {0};

/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static void
donothingReceive(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
}

static void
donothingSetup(DestReceiver *self, TupleDesc typeinfo)
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
	debugtup, donothingSetup, donothingCleanup
};
static DestReceiver spi_printtupDR = {
	spi_printtup, donothingSetup, donothingCleanup
};

/* ----------------
 *		BeginCommand - prepare destination for tuples of the given type
 * ----------------
 */
void
BeginCommand(char *pname,
			 int operation,
			 TupleDesc tupdesc,
			 bool isIntoRel,
			 bool isIntoPortal,
			 char *tag,
			 CommandDest dest)
{
	Form_pg_attribute *attrs = tupdesc->attrs;
	int			natts = tupdesc->natts;
	int			i;

	switch (dest)
	{
		case Remote:
		case RemoteInternal:
			/* ----------------
			 *		if this is a "retrieve into portal" query, done
			 *		because nothing needs to be sent to the fe.
			 * ----------------
			 */
			CommandInfo[0] = '\0';
			if (isIntoPortal)
				break;

			/* ----------------
			 *		if portal name not specified for remote query,
			 *		use the "blank" portal.
			 * ----------------
			 */
			if (pname == NULL)
				pname = "blank";

			/* ----------------
			 *		send fe info on tuples we're about to send
			 * ----------------
			 */
			pq_puttextmessage('P', pname);

			/* ----------------
			 *		if this is a retrieve, then we send back the tuple
			 *		descriptor of the tuples.  "retrieve into" is an
			 *		exception because no tuples are returned in that case.
			 * ----------------
			 */
			if (operation == CMD_SELECT && !isIntoRel)
			{
				StringInfoData buf;

				pq_beginmessage(&buf);
				pq_sendbyte(&buf, 'T'); /* tuple descriptor message type */
				pq_sendint(&buf, natts, 2);		/* # of attributes in
												 * tuples */

				for (i = 0; i < natts; ++i)
				{
					pq_sendstring(&buf, NameStr(attrs[i]->attname));
					pq_sendint(&buf, (int) attrs[i]->atttypid,
							   sizeof(attrs[i]->atttypid));
					pq_sendint(&buf, attrs[i]->attlen,
							   sizeof(attrs[i]->attlen));
					if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
						pq_sendint(&buf, attrs[i]->atttypmod,
								   sizeof(attrs[i]->atttypmod));
				}
				pq_endmessage(&buf);
			}
			break;

		case Debug:
			/* ----------------
			 *		show the return type of the tuples
			 * ----------------
			 */
			if (pname == NULL)
				pname = "blank";

			showatts(pname, tupdesc);
			break;

		case None:
		default:
			break;
	}
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
		default:
			return &donothingDR;
	}

	/*
	 * never gets here, but DECstation lint appears to be stupid...
	 */

	return &donothingDR;
}

/* ----------------
 *		EndCommand - tell destination that no more tuples will arrive
 * ----------------
 */
void
EndCommand(char *commandTag, CommandDest dest)
{
	char		buf[64];

	switch (dest)
	{
		case Remote:
		case RemoteInternal:
			/* ----------------
			 *		tell the fe that the query is over
			 * ----------------
			 */
			sprintf(buf, "%s%s", commandTag, CommandInfo);
			pq_puttextmessage('C', buf);
			CommandInfo[0] = '\0';
			break;

		case Debug:
		case None:
		default:
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
			/* ----------------
			 *		tell the fe that we saw an empty query string
			 * ----------------
			 */
			pq_putbytes("I", 2);/* note we send I and \0 */
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

void
UpdateCommandInfo(int operation, Oid lastoid, uint32 tuples)
{
	switch (operation)
	{
			case CMD_INSERT:
			if (tuples > 1)
				lastoid = InvalidOid;
			sprintf(CommandInfo, " %u %u", lastoid, tuples);
			break;
		case CMD_DELETE:
		case CMD_UPDATE:
			sprintf(CommandInfo, " %u", tuples);
			break;
		default:
			CommandInfo[0] = '\0';
			break;
	}
}
