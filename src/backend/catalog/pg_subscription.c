/*-------------------------------------------------------------------------
 *
 * pg_subscription.c
 *		replication subscriptions
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_subscription.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"

#include "catalog/pg_type.h"
#include "catalog/pg_subscription.h"

#include "nodes/makefuncs.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


static List *textarray_to_stringlist(ArrayType *textarray);

/*
 * Fetch the subscription from the syscache.
 */
Subscription *
GetSubscription(Oid subid, bool missing_ok)
{
	HeapTuple		tup;
	Subscription   *sub;
	Form_pg_subscription	subform;
	Datum			datum;
	bool			isnull;

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
	sub->name = pstrdup(NameStr(subform->subname));
	sub->owner = subform->subowner;
	sub->enabled = subform->subenabled;

	/* Get conninfo */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID,
							tup,
							Anum_pg_subscription_subconninfo,
							&isnull);
	Assert(!isnull);
	sub->conninfo = pstrdup(TextDatumGetCString(datum));

	/* Get slotname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID,
							tup,
							Anum_pg_subscription_subslotname,
							&isnull);
	Assert(!isnull);
	sub->slotname = pstrdup(NameStr(*DatumGetName(datum)));

	/* Get publications */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID,
							tup,
							Anum_pg_subscription_subpublications,
							&isnull);
	Assert(!isnull);
	sub->publications = textarray_to_stringlist(DatumGetArrayTypeP(datum));

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
	int				nsubs = 0;
	Relation		rel;
	ScanKeyData		scankey;
	SysScanDesc		scan;
	HeapTuple		tup;

	rel = heap_open(SubscriptionRelationId, RowExclusiveLock);

	ScanKeyInit(&scankey,
				Anum_pg_subscription_subdbid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		nsubs++;

	systable_endscan(scan);

	heap_close(rel, NoLock);

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
	pfree(sub->slotname);
	list_free_deep(sub->publications);
	pfree(sub);
}

/*
 * get_subscription_oid - given a subscription name, look up the OID
 *
 * If missing_ok is false, throw an error if name not found.  If true, just
 * return InvalidOid.
 */
Oid
get_subscription_oid(const char *subname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid2(SUBSCRIPTIONNAME, MyDatabaseId,
						  CStringGetDatum(subname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist", subname)));
	return oid;
}

/*
 * get_subscription_name - given a subscription OID, look up the name
 */
char *
get_subscription_name(Oid subid)
{
	HeapTuple		tup;
	char		   *subname;
	Form_pg_subscription subform;

	tup = SearchSysCache1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for subscription %u", subid);

	subform = (Form_pg_subscription) GETSTRUCT(tup);
	subname = pstrdup(NameStr(subform->subname));

	ReleaseSysCache(tup);

	return subname;
}

/*
 * Convert text array to list of strings.
 *
 * Note: the resulting list of strings is pallocated here.
 */
static List *
textarray_to_stringlist(ArrayType *textarray)
{
	Datum		   *elems;
	int				nelems, i;
	List		   *res = NIL;

	deconstruct_array(textarray,
					  TEXTOID, -1, false, 'i',
					  &elems, NULL, &nelems);

	if (nelems == 0)
		return NIL;

	for (i = 0; i < nelems; i++)
		res = lappend(res, makeString(pstrdup(TextDatumGetCString(elems[i]))));

	return res;
}
