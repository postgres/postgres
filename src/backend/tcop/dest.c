/*-------------------------------------------------------------------------
 *
 * dest.c--
 *	  support for various communication destinations - see lib/H/tcop/dest.h
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/dest.c,v 1.20 1998/05/19 18:05:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - prepare destination for tuples of the given type
 *		EndCommand - tell destination that no more tuples will arrive
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 *
 */
#include <stdio.h>				/* for sprintf() */
#include <string.h>

#include "postgres.h"

#include "access/htup.h"
#include "libpq/libpq.h"
#include "access/printtup.h"
#include "utils/portal.h"
#include "utils/palloc.h"

#include "executor/executor.h"

#include "tcop/dest.h"

#include "catalog/pg_type.h"
#include "utils/mcxt.h"

#include "commands/async.h"

static char CommandInfo[32] = {0};

/* ----------------
 *		output functions
 * ----------------
 */
static void
donothing(HeapTuple tuple, TupleDesc attrdesc)
{
}

extern void spi_printtup(HeapTuple tuple, TupleDesc tupdesc);

void		(*
			 DestToFunction(CommandDest dest)) (HeapTuple, TupleDesc)
{
	switch (dest)
	{
			case RemoteInternal:
			return printtup_internal;
			break;

		case Remote:
			return printtup;
			break;

		case Local:
			return be_printtup;
			break;

		case Debug:
			return debugtup;
			break;

		case SPI:
			return spi_printtup;
			break;

		case None:
		default:
			return donothing;
			break;
	}

	/*
	 * never gets here, but DECstation lint appears to be stupid...
	 */

	return donothing;
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
		case RemoteInternal:
		case Remote:
			/* ----------------
			 *		tell the fe that the query is over
			 * ----------------
			 */
			pq_putnchar("C", 1);
			sprintf(buf, "%s%s", commandTag, CommandInfo);
			CommandInfo[0] = 0;
			pq_putstr(buf);
			break;

		case Local:
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
		pq_putnchar("H", 1);	/* new way */
	else
		pq_putnchar("B", 1);	/* old way */
}

void
ReceiveCopyBegin(void)
{
	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
		pq_putnchar("G", 1);	/* new way */
	else
		pq_putnchar("D", 1);	/* old way */
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
			{
				/* ----------------
				 *		tell the fe that we saw an empty query string
				 * ----------------
				 */
				pq_putstr("I");
			}
			break;

		case Local:
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
			{
				if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
					pq_putnchar("Z", 1);
				/* Flush output at end of cycle in any case. */
				pq_flush();
			}
			break;

		case Local:
		case Debug:
		case None:
		default:
			break;
	}
}

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
	PortalEntry *entry;
	AttributeTupleForm *attrs = tupdesc->attrs;
	int			natts = tupdesc->natts;
	int			i;
	char	   *p;

	switch (dest)
	{
		case RemoteInternal:
		case Remote:
			/* ----------------
			 *		if this is a "retrieve portal" query, just return
			 *		because nothing needs to be sent to the fe.
			 * ----------------
			 */
			CommandInfo[0] = 0;
			if (isIntoPortal)
				return;

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
			pq_putnchar("P", 1);/* new portal.. */
			pq_putstr(pname);	/* portal name */

			/* ----------------
			 *		if this is a retrieve, then we send back the tuple
			 *		descriptor of the tuples.  "retrieve into" is an
			 *		exception because no tuples are returned in that case.
			 * ----------------
			 */
			if (operation == CMD_SELECT && !isIntoRel)
			{
				pq_putnchar("T", 1);	/* type info to follow.. */
				pq_putint(natts, 2);	/* number of attributes in tuples */

				for (i = 0; i < natts; ++i)
				{
					pq_putstr(attrs[i]->attname.data);	/* if 16 char name
														 * oops.. */
					pq_putint((int) attrs[i]->atttypid, sizeof(attrs[i]->atttypid));
					pq_putint(attrs[i]->attlen, sizeof(attrs[i]->attlen));
					if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 2)
						pq_putint(attrs[i]->atttypmod, sizeof(attrs[i]->atttypmod));
				}
			}
			break;

		case Local:
			/* ----------------
			 *		prepare local portal buffer for query results
			 *		and setup result for PQexec()
			 * ----------------
			 */
			entry = be_currentportal();
			if (pname != NULL)
				pbuf_setportalinfo(entry, pname);

			if (operation == CMD_SELECT && !isIntoRel)
			{
				be_typeinit(entry, tupdesc, natts);
				p = (char *) palloc(strlen(entry->name) + 2);
				p[0] = 'P';
				strcpy(p + 1, entry->name);
			}
			else
			{
				p = (char *) palloc(strlen(tag) + 2);
				p[0] = 'C';
				strcpy(p + 1, tag);
			}
			entry->result = p;
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
			CommandInfo[0] = 0;
	}
	return;
}
