/* -------------------------------------------------------------------------
 *
 * pg_subscription.h
 *	  definition of the "subscription" system catalog (pg_subscription)
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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

#include "catalog/genbki.h"
#include "catalog/pg_subscription_d.h"

#include "nodes/pg_list.h"

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
 * NOTE:  When adding a column, also update system_views.sql.
 */
CATALOG(pg_subscription,6100,SubscriptionRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(6101,SubscriptionRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;			/* oid */

	Oid			subdbid BKI_LOOKUP(pg_database);	/* Database the
													 * subscription is in. */
	NameData	subname;		/* Name of the subscription */

	Oid			subowner BKI_LOOKUP(pg_authid); /* Owner of the subscription */

	bool		subenabled;		/* True if the subscription is enabled (the
								 * worker should be running) */

	bool		subbinary;		/* True if the subscription wants the
								 * publisher to send data in binary */

	bool		substream;		/* Stream in-progress transactions. */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* Connection string to the publisher */
	text		subconninfo BKI_FORCE_NOT_NULL;

	/* Slot name on publisher */
	NameData	subslotname BKI_FORCE_NULL;

	/* Synchronous commit setting for worker */
	text		subsynccommit BKI_FORCE_NOT_NULL;

	/* List of publications subscribed to */
	text		subpublications[1] BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_subscription;

typedef FormData_pg_subscription *Form_pg_subscription;

DECLARE_TOAST(pg_subscription, 4183, 4184);
#define PgSubscriptionToastTable 4183
#define PgSubscriptionToastIndex 4184

DECLARE_UNIQUE_INDEX_PKEY(pg_subscription_oid_index, 6114, on pg_subscription using btree(oid oid_ops));
#define SubscriptionObjectIndexId 6114
DECLARE_UNIQUE_INDEX(pg_subscription_subname_index, 6115, on pg_subscription using btree(subdbid oid_ops, subname name_ops));
#define SubscriptionNameIndexId 6115

typedef struct Subscription
{
	Oid			oid;			/* Oid of the subscription */
	Oid			dbid;			/* Oid of the database which subscription is
								 * in */
	char	   *name;			/* Name of the subscription */
	Oid			owner;			/* Oid of the subscription owner */
	bool		enabled;		/* Indicates if the subscription is enabled */
	bool		binary;			/* Indicates if the subscription wants data in
								 * binary format */
	bool		stream;			/* Allow streaming in-progress transactions. */
	char	   *conninfo;		/* Connection string to the publisher */
	char	   *slotname;		/* Name of the replication slot */
	char	   *synccommit;		/* Synchronous commit setting for worker */
	List	   *publications;	/* List of publication names to subscribe to */
} Subscription;

extern Subscription *GetSubscription(Oid subid, bool missing_ok);
extern void FreeSubscription(Subscription *sub);
extern Oid	get_subscription_oid(const char *subname, bool missing_ok);
extern char *get_subscription_name(Oid subid, bool missing_ok);

extern int	CountDBSubscriptions(Oid dbid);

#endif							/* PG_SUBSCRIPTION_H */
