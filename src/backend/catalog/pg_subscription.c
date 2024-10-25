/*-------------------------------------------------------------------------
 *
 * pg_subscription.c
 *		replication subscriptions
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_subscription.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static List *textarray_to_stringlist(ArrayType *textarray);

/*
 * Add a comma-separated list of publication names to the 'dest' string.
 */
void
GetPublicationsStr(List *publications, StringInfo dest, bool quote_literal)
{
	ListCell   *lc;
	bool		first = true;

	Assert(publications != NIL);

	foreach(lc, publications)
	{
		char	   *pubname = strVal(lfirst(lc));

		if (first)
			first = false;
		else
			appendStringInfoString(dest, ", ");

		if (quote_literal)
			appendStringInfoString(dest, quote_literal_cstr(pubname));
		else
		{
			appendStringInfoChar(dest, '"');
			appendStringInfoString(dest, pubname);
			appendStringInfoChar(dest, '"');
		}
	}
}

/*
 * Fetch the subscription from the syscache.
 */
Subscription *
GetSubscription(Oid subid, bool missing_ok)
{
	HeapTuple	tup;
	Subscription *sub;
	Form_pg_subscription subform;
	Datum		datum;
	bool		isnull;

	tup = SearchSysCache1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
	{
		if (missing_ok)
			return NULL;

		elog(ERROR, "cache lookup failed for subscription %u", subid);
	}

	subform = (Form_pg_subscription) GETSTRUCT(tup);

	sub = (Subscription *) palloc(sizeof(Subscription));
	sub->oid = subid;
	sub->dbid = subform->subdbid;
	sub->skiplsn = subform->subskiplsn;
	sub->name = pstrdup(NameStr(subform->subname));
	sub->owner = subform->subowner;
	sub->enabled = subform->subenabled;
	sub->binary = subform->subbinary;
	sub->stream = subform->substream;
	sub->twophasestate = subform->subtwophasestate;
	sub->disableonerr = subform->subdisableonerr;
	sub->passwordrequired = subform->subpasswordrequired;
	sub->runasowner = subform->subrunasowner;
	sub->failover = subform->subfailover;

	/* Get conninfo */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_subconninfo);
	sub->conninfo = TextDatumGetCString(datum);

	/* Get slotname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID,
							tup,
							Anum_pg_subscription_subslotname,
							&isnull);
	if (!isnull)
		sub->slotname = pstrdup(NameStr(*DatumGetName(datum)));
	else
		sub->slotname = NULL;

	/* Get synccommit */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_subsynccommit);
	sub->synccommit = TextDatumGetCString(datum);

	/* Get publications */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_subpublications);
	sub->publications = textarray_to_stringlist(DatumGetArrayTypeP(datum));

	/* Get origin */
	datum = SysCacheGetAttrNotNull(SUBSCRIPTIONOID,
								   tup,
								   Anum_pg_subscription_suborigin);
	sub->origin = TextDatumGetCString(datum);

	/* Is the subscription owner a superuser? */
	sub->ownersuperuser = superuser_arg(sub->owner);

	ReleaseSysCache(tup);

	return sub;
}

/*
 * Return number of subscriptions defined in given database.
 * Used by dropdb() to check if database can indeed be dropped.
 */
int
CountDBSubscriptions(Oid dbid)
{
	int			nsubs = 0;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	HeapTuple	tup;

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_pg_subscription_subdbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		nsubs++;

	systable_endscan(scan);

	table_close(rel, NoLock);

	return nsubs;
}

/*
 * Free memory allocated by subscription struct.
 */
void
FreeSubscription(Subscription *sub)
{
	pfree(sub->name);
	pfree(sub->conninfo);
	if (sub->slotname)
		pfree(sub->slotname);
	list_free_deep(sub->publications);
	pfree(sub);
}

/*
 * Disable the given subscription.
 */
void
DisableSubscription(Oid subid)
{
	Relation	rel;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	HeapTuple	tup;

	/* Look up the subscription in the catalog */
	rel = table_open(SubscriptionRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for subscription %u", subid);

	LockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);

	/* Form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	/* Set the subscription to disabled. */
	values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(false);
	replaces[Anum_pg_subscription_subenabled - 1] = true;

	/* Update the catalog */
	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);
	heap_freetuple(tup);

	table_close(rel, NoLock);
}

/*
 * Convert text array to list of strings.
 *
 * Note: the resulting list of strings is pallocated here.
 */
static List *
textarray_to_stringlist(ArrayType *textarray)
{
	Datum	   *elems;
	int			nelems,
				i;
	List	   *res = NIL;

	deconstruct_array_builtin(textarray, TEXTOID, &elems, NULL, &nelems);

	if (nelems == 0)
		return NIL;

	for (i = 0; i < nelems; i++)
		res = lappend(res, makeString(TextDatumGetCString(elems[i])));

	return res;
}

/*
 * Add new state record for a subscription table.
 *
 * If retain_lock is true, then don't release the locks taken in this function.
 * We normally release the locks at the end of transaction but in binary-upgrade
 * mode, we expect to release those immediately.
 */
void
AddSubscriptionRelState(Oid subid, Oid relid, char state,
						XLogRecPtr sublsn, bool retain_lock)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription_rel];
	Datum		values[Natts_pg_subscription_rel];

	LockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);

	rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

	/* Try finding existing mapping. */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONRELMAP,
							  ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(subid));
	if (HeapTupleIsValid(tup))
		elog(ERROR, "subscription table %u in subscription %u already exists",
			 relid, subid);

	/* Form the tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_subscription_rel_srsubid - 1] = ObjectIdGetDatum(subid);
	values[Anum_pg_subscription_rel_srrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_subscription_rel_srsubstate - 1] = CharGetDatum(state);
	if (sublsn != InvalidXLogRecPtr)
		values[Anum_pg_subscription_rel_srsublsn - 1] = LSNGetDatum(sublsn);
	else
		nulls[Anum_pg_subscription_rel_srsublsn - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);

	/* Cleanup. */
	if (retain_lock)
	{
		table_close(rel, NoLock);
	}
	else
	{
		table_close(rel, RowExclusiveLock);
		UnlockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);
	}
}

/*
 * Update the state of a subscription table.
 */
void
UpdateSubscriptionRelState(Oid subid, Oid relid, char state,
						   XLogRecPtr sublsn)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription_rel];
	Datum		values[Natts_pg_subscription_rel];
	bool		replaces[Natts_pg_subscription_rel];

	LockSharedObject(SubscriptionRelationId, subid, 0, AccessShareLock);

	rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

	/* Try finding existing mapping. */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONRELMAP,
							  ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(subid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "subscription table %u in subscription %u does not exist",
			 relid, subid);

	/* Update the tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	replaces[Anum_pg_subscription_rel_srsubstate - 1] = true;
	values[Anum_pg_subscription_rel_srsubstate - 1] = CharGetDatum(state);

	replaces[Anum_pg_subscription_rel_srsublsn - 1] = true;
	if (sublsn != InvalidXLogRecPtr)
		values[Anum_pg_subscription_rel_srsublsn - 1] = LSNGetDatum(sublsn);
	else
		nulls[Anum_pg_subscription_rel_srsublsn - 1] = true;

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);

	/* Update the catalog. */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Cleanup. */
	table_close(rel, NoLock);
}

/*
 * Get state of subscription table.
 *
 * Returns SUBREL_STATE_UNKNOWN when the table is not in the subscription.
 */
char
GetSubscriptionRelState(Oid subid, Oid relid, XLogRecPtr *sublsn)
{
	HeapTuple	tup;
	char		substate;
	bool		isnull;
	Datum		d;
	Relation	rel;

	/*
	 * This is to avoid the race condition with AlterSubscription which tries
	 * to remove this relstate.
	 */
	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	/* Try finding the mapping. */
	tup = SearchSysCache2(SUBSCRIPTIONRELMAP,
						  ObjectIdGetDatum(relid),
						  ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, AccessShareLock);
		*sublsn = InvalidXLogRecPtr;
		return SUBREL_STATE_UNKNOWN;
	}

	/* Get the state. */
	substate = ((Form_pg_subscription_rel) GETSTRUCT(tup))->srsubstate;

	/* Get the LSN */
	d = SysCacheGetAttr(SUBSCRIPTIONRELMAP, tup,
						Anum_pg_subscription_rel_srsublsn, &isnull);
	if (isnull)
		*sublsn = InvalidXLogRecPtr;
	else
		*sublsn = DatumGetLSN(d);

	/* Cleanup */
	ReleaseSysCache(tup);

	table_close(rel, AccessShareLock);

	return substate;
}

/*
 * Drop subscription relation mapping. These can be for a particular
 * subscription, or for a particular relation, or both.
 */
void
RemoveSubscriptionRel(Oid subid, Oid relid)
{
	Relation	rel;
	TableScanDesc scan;
	ScanKeyData skey[2];
	HeapTuple	tup;
	int			nkeys = 0;

	rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

	if (OidIsValid(subid))
	{
		ScanKeyInit(&skey[nkeys++],
					Anum_pg_subscription_rel_srsubid,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(subid));
	}

	if (OidIsValid(relid))
	{
		ScanKeyInit(&skey[nkeys++],
					Anum_pg_subscription_rel_srrelid,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(relid));
	}

	/* Do the search and delete what we found. */
	scan = table_beginscan_catalog(rel, nkeys, skey);
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription_rel subrel;

		subrel = (Form_pg_subscription_rel) GETSTRUCT(tup);

		/*
		 * We don't allow to drop the relation mapping when the table
		 * synchronization is in progress unless the caller updates the
		 * corresponding subscription as well. This is to ensure that we don't
		 * leave tablesync slots or origins in the system when the
		 * corresponding table is dropped.
		 */
		if (!OidIsValid(subid) && subrel->srsubstate != SUBREL_STATE_READY)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not drop relation mapping for subscription \"%s\"",
							get_subscription_name(subrel->srsubid, false)),
					 errdetail("Table synchronization for relation \"%s\" is in progress and is in state \"%c\".",
							   get_rel_name(relid), subrel->srsubstate),

			/*
			 * translator: first %s is a SQL ALTER command and second %s is a
			 * SQL DROP command
			 */
					 errhint("Use %s to enable subscription if not already enabled or use %s to drop the subscription.",
							 "ALTER SUBSCRIPTION ... ENABLE",
							 "DROP SUBSCRIPTION ...")));
		}

		CatalogTupleDelete(rel, &tup->t_self);
	}
	table_endscan(scan);

	table_close(rel, RowExclusiveLock);
}

/*
 * Does the subscription have any relations?
 *
 * Use this function only to know true/false, and when you have no need for the
 * List returned by GetSubscriptionRelations.
 */
bool
HasSubscriptionRelations(Oid subid)
{
	Relation	rel;
	ScanKeyData skey[1];
	SysScanDesc scan;
	bool		has_subrels;

	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_subscription_rel_srsubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(subid));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, skey);

	/* If even a single tuple exists then the subscription has tables. */
	has_subrels = HeapTupleIsValid(systable_getnext(scan));

	/* Cleanup */
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return has_subrels;
}

/*
 * Get the relations for the subscription.
 *
 * If not_ready is true, return only the relations that are not in a ready
 * state, otherwise return all the relations of the subscription.  The
 * returned list is palloc'ed in the current memory context.
 */
List *
GetSubscriptionRelations(Oid subid, bool not_ready)
{
	List	   *res = NIL;
	Relation	rel;
	HeapTuple	tup;
	int			nkeys = 0;
	ScanKeyData skey[2];
	SysScanDesc scan;

	rel = table_open(SubscriptionRelRelationId, AccessShareLock);

	ScanKeyInit(&skey[nkeys++],
				Anum_pg_subscription_rel_srsubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(subid));

	if (not_ready)
		ScanKeyInit(&skey[nkeys++],
					Anum_pg_subscription_rel_srsubstate,
					BTEqualStrategyNumber, F_CHARNE,
					CharGetDatum(SUBREL_STATE_READY));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, nkeys, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_subscription_rel subrel;
		SubscriptionRelState *relstate;
		Datum		d;
		bool		isnull;

		subrel = (Form_pg_subscription_rel) GETSTRUCT(tup);

		relstate = (SubscriptionRelState *) palloc(sizeof(SubscriptionRelState));
		relstate->relid = subrel->srrelid;
		relstate->state = subrel->srsubstate;
		d = SysCacheGetAttr(SUBSCRIPTIONRELMAP, tup,
							Anum_pg_subscription_rel_srsublsn, &isnull);
		if (isnull)
			relstate->lsn = InvalidXLogRecPtr;
		else
			relstate->lsn = DatumGetLSN(d);

		res = lappend(res, relstate);
	}

	/* Cleanup */
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return res;
}
