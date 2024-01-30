/* -------------------------------------------------------------------------
 *
 * pg_subscription.h
 *	  definition of the "subscription" system catalog (pg_subscription)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_subscription.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SUBSCRIPTION_H
#define PG_SUBSCRIPTION_H

#include "access/xlogdefs.h"
#include "catalog/genbki.h"
#include "catalog/pg_subscription_d.h"

#include "nodes/pg_list.h"

/*
 * two_phase tri-state values. See comments atop worker.c to know more about
 * these states.
 */
#define LOGICALREP_TWOPHASE_STATE_DISABLED 'd'
#define LOGICALREP_TWOPHASE_STATE_PENDING 'p'
#define LOGICALREP_TWOPHASE_STATE_ENABLED 'e'

/*
 * The subscription will request the publisher to only send changes that do not
 * have any origin.
 */
#define LOGICALREP_ORIGIN_NONE "none"

/*
 * The subscription will request the publisher to send changes regardless
 * of their origin.
 */
#define LOGICALREP_ORIGIN_ANY "any"

/* ----------------
 *		pg_subscription definition. cpp turns this into
 *		typedef struct FormData_pg_subscription
 * ----------------
 */

/*
 * Technically, the subscriptions live inside the database, so a shared catalog
 * seems weird, but the replication launcher process needs to access all of
 * them to be able to start the workers, so we have to put them in a shared,
 * nailed catalog.
 *
 * CAUTION:  There is a GRANT in system_views.sql to grant public select
 * access on all columns except subconninfo.  When you add a new column
 * here, be sure to update that (or, if the new column is not to be publicly
 * readable, update associated comments and catalogs.sgml instead).
 */
CATALOG(pg_subscription,6100,SubscriptionRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(6101,SubscriptionRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* oid */

	Oid			subdbid BKI_LOOKUP(pg_database);	/* Database the
													 * subscription is in. */

	XLogRecPtr	subskiplsn;		/* All changes finished at this LSN are
								 * skipped */

	NameData	subname;		/* Name of the subscription */

	Oid			subowner BKI_LOOKUP(pg_authid); /* Owner of the subscription */

	bool		subenabled;		/* True if the subscription is enabled (the
								 * worker should be running) */

	bool		subbinary;		/* True if the subscription wants the
								 * publisher to send data in binary */

	char		substream;		/* Stream in-progress transactions. See
								 * LOGICALREP_STREAM_xxx constants. */

	char		subtwophasestate;	/* Stream two-phase transactions */

	bool		subdisableonerr;	/* True if a worker error should cause the
									 * subscription to be disabled */

	bool		subpasswordrequired;	/* Must connection use a password? */

	bool		subrunasowner;	/* True if replication should execute as the
								 * subscription owner */

	bool		subfailover;	/* True if the associated replication slots
								 * (i.e. the main slot and the table sync
								 * slots) in the upstream database are enabled
								 * to be synchronized to the standbys. */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* Connection string to the publisher */
	text		subconninfo BKI_FORCE_NOT_NULL;

	/* Slot name on publisher */
	NameData	subslotname BKI_FORCE_NULL;

	/* Synchronous commit setting for worker */
	text		subsynccommit BKI_FORCE_NOT_NULL;

	/* List of publications subscribed to */
	text		subpublications[1] BKI_FORCE_NOT_NULL;

	/* Only publish data originating from the specified origin */
	text		suborigin BKI_DEFAULT(LOGICALREP_ORIGIN_ANY);
#endif
} FormData_pg_subscription;

typedef FormData_pg_subscription *Form_pg_subscription;

DECLARE_TOAST_WITH_MACRO(pg_subscription, 4183, 4184, PgSubscriptionToastTable, PgSubscriptionToastIndex);

DECLARE_UNIQUE_INDEX_PKEY(pg_subscription_oid_index, 6114, SubscriptionObjectIndexId, pg_subscription, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_subscription_subname_index, 6115, SubscriptionNameIndexId, pg_subscription, btree(subdbid oid_ops, subname name_ops));

MAKE_SYSCACHE(SUBSCRIPTIONOID, pg_subscription_oid_index, 4);
MAKE_SYSCACHE(SUBSCRIPTIONNAME, pg_subscription_subname_index, 4);

typedef struct Subscription
{
	Oid			oid;			/* Oid of the subscription */
	Oid			dbid;			/* Oid of the database which subscription is
								 * in */
	XLogRecPtr	skiplsn;		/* All changes finished at this LSN are
								 * skipped */
	char	   *name;			/* Name of the subscription */
	Oid			owner;			/* Oid of the subscription owner */
	bool		ownersuperuser; /* Is the subscription owner a superuser? */
	bool		enabled;		/* Indicates if the subscription is enabled */
	bool		binary;			/* Indicates if the subscription wants data in
								 * binary format */
	char		stream;			/* Allow streaming in-progress transactions.
								 * See LOGICALREP_STREAM_xxx constants. */
	char		twophasestate;	/* Allow streaming two-phase transactions */
	bool		disableonerr;	/* Indicates if the subscription should be
								 * automatically disabled if a worker error
								 * occurs */
	bool		passwordrequired;	/* Must connection use a password? */
	bool		runasowner;		/* Run replication as subscription owner */
	bool		failover;		/* True if the associated replication slots
								 * (i.e. the main slot and the table sync
								 * slots) in the upstream database are enabled
								 * to be synchronized to the standbys. */
	char	   *conninfo;		/* Connection string to the publisher */
	char	   *slotname;		/* Name of the replication slot */
	char	   *synccommit;		/* Synchronous commit setting for worker */
	List	   *publications;	/* List of publication names to subscribe to */
	char	   *origin;			/* Only publish data originating from the
								 * specified origin */
} Subscription;

/* Disallow streaming in-progress transactions. */
#define LOGICALREP_STREAM_OFF 'f'

/*
 * Streaming in-progress transactions are written to a temporary file and
 * applied only after the transaction is committed on upstream.
 */
#define LOGICALREP_STREAM_ON 't'

/*
 * Streaming in-progress transactions are applied immediately via a parallel
 * apply worker.
 */
#define LOGICALREP_STREAM_PARALLEL 'p'

extern Subscription *GetSubscription(Oid subid, bool missing_ok);
extern void FreeSubscription(Subscription *sub);
extern void DisableSubscription(Oid subid);

extern int	CountDBSubscriptions(Oid dbid);

#endif							/* PG_SUBSCRIPTION_H */
