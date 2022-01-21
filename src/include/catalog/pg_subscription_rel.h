/* -------------------------------------------------------------------------
 *
 * pg_subscription_rel.h
 *	  definition of the system catalog containing the state for each
 *	  replicated table in each subscription (pg_subscription_rel)
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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

#include "access/xlogdefs.h"
#include "catalog/genbki.h"
#include "catalog/pg_subscription_rel_d.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_subscription_rel definition. cpp turns this into
 *		typedef struct FormData_pg_subscription_rel
 * ----------------
 */
CATALOG(pg_subscription_rel,6102,SubscriptionRelRelationId)
{
	Oid			srsubid BKI_LOOKUP(pg_subscription);	/* Oid of subscription */
	Oid			srrelid BKI_LOOKUP(pg_class);	/* Oid of relation */
	char		srsubstate;		/* state of the relation in subscription */

	/*
	 * Although srsublsn is a fixed-width type, it is allowed to be NULL, so
	 * we prevent direct C code access to it just as for a varlena field.
	 */
#ifdef CATALOG_VARLEN			/* variable-length fields start here */

	XLogRecPtr	srsublsn BKI_FORCE_NULL;	/* remote LSN of the state change
											 * used for synchronization
											 * coordination, or NULL if not
											 * valid */
#endif
} FormData_pg_subscription_rel;

typedef FormData_pg_subscription_rel *Form_pg_subscription_rel;

DECLARE_UNIQUE_INDEX_PKEY(pg_subscription_rel_srrelid_srsubid_index, 6117, SubscriptionRelSrrelidSrsubidIndexId, on pg_subscription_rel using btree(srrelid oid_ops, srsubid oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

/* ----------------
 *		substate constants
 * ----------------
 */
#define SUBREL_STATE_INIT		'i' /* initializing (sublsn NULL) */
#define SUBREL_STATE_DATASYNC	'd' /* data is being synchronized (sublsn
									 * NULL) */
#define SUBREL_STATE_FINISHEDCOPY 'f'	/* tablesync copy phase is completed
										 * (sublsn NULL) */
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
extern char GetSubscriptionRelState(Oid subid, Oid relid, XLogRecPtr *sublsn);
extern void RemoveSubscriptionRel(Oid subid, Oid relid);

extern bool HasSubscriptionRelations(Oid subid);
extern List *GetSubscriptionRelations(Oid subid);
extern List *GetSubscriptionNotReadyRelations(Oid subid);

#endif							/* PG_SUBSCRIPTION_REL_H */
