/* -------------------------------------------------------------------------
 *
 * pg_subscription_rel.h
 *	  definition of the system catalog containing the state for each
 *	  replicated table in each subscription (pg_subscription_rel)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_subscription_rel.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SUBSCRIPTION_REL_H
#define PG_SUBSCRIPTION_REL_H

#include "catalog/genbki.h"
#include "catalog/pg_subscription_rel_d.h"

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_subscription_rel definition. cpp turns this into
 *		typedef struct FormData_pg_subscription_rel
 * ----------------
 */
CATALOG(pg_subscription_rel,6102,SubscriptionRelRelationId)
{
	Oid			srsubid;		/* Oid of subscription */
	Oid			srrelid;		/* Oid of relation */
	char		srsubstate;		/* state of the relation in subscription */
	XLogRecPtr	srsublsn;		/* remote lsn of the state change used for
								 * synchronization coordination */
} FormData_pg_subscription_rel;

typedef FormData_pg_subscription_rel *Form_pg_subscription_rel;

#ifdef EXPOSE_TO_CLIENT_CODE

/* ----------------
 *		substate constants
 * ----------------
 */
#define SUBREL_STATE_INIT		'i' /* initializing (sublsn NULL) */
#define SUBREL_STATE_DATASYNC	'd' /* data is being synchronized (sublsn
									 * NULL) */
#define SUBREL_STATE_SYNCDONE	's' /* synchronization finished in front of
									 * apply (sublsn set) */
#define SUBREL_STATE_READY		'r' /* ready (sublsn set) */

/* These are never stored in the catalog, we only use them for IPC. */
#define SUBREL_STATE_UNKNOWN	'\0'	/* unknown state */
#define SUBREL_STATE_SYNCWAIT	'w' /* waiting for sync */
#define SUBREL_STATE_CATCHUP	'c' /* catching up with apply */

#endif							/* EXPOSE_TO_CLIENT_CODE */

typedef struct SubscriptionRelState
{
	Oid			relid;
	XLogRecPtr	lsn;
	char		state;
} SubscriptionRelState;

extern void AddSubscriptionRelState(Oid subid, Oid relid, char state,
									XLogRecPtr sublsn);
extern void UpdateSubscriptionRelState(Oid subid, Oid relid, char state,
									   XLogRecPtr sublsn);
extern char GetSubscriptionRelState(Oid subid, Oid relid,
									XLogRecPtr *sublsn, bool missing_ok);
extern void RemoveSubscriptionRel(Oid subid, Oid relid);

extern List *GetSubscriptionRelations(Oid subid);
extern List *GetSubscriptionNotReadyRelations(Oid subid);

#endif							/* PG_SUBSCRIPTION_REL_H */
