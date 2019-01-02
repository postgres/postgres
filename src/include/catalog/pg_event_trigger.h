/*-------------------------------------------------------------------------
 *
 * pg_event_trigger.h
 *	  definition of the "event trigger" system catalog (pg_event_trigger)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_event_trigger.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EVENT_TRIGGER_H
#define PG_EVENT_TRIGGER_H

#include "catalog/genbki.h"
#include "catalog/pg_event_trigger_d.h"

/* ----------------
 *		pg_event_trigger definition.    cpp turns this into
 *		typedef struct FormData_pg_event_trigger
 * ----------------
 */
CATALOG(pg_event_trigger,3466,EventTriggerRelationId)
{
	Oid			oid;			/* oid */
	NameData	evtname;		/* trigger's name */
	NameData	evtevent;		/* trigger's event */
	Oid			evtowner;		/* trigger's owner */
	Oid			evtfoid;		/* OID of function to be called */
	char		evtenabled;		/* trigger's firing configuration WRT
								 * session_replication_role */

#ifdef CATALOG_VARLEN
	text		evttags[1];		/* command TAGs this event trigger targets */
#endif
} FormData_pg_event_trigger;

/* ----------------
 *		Form_pg_event_trigger corresponds to a pointer to a tuple with
 *		the format of pg_event_trigger relation.
 * ----------------
 */
typedef FormData_pg_event_trigger *Form_pg_event_trigger;

#endif							/* PG_EVENT_TRIGGER_H */
