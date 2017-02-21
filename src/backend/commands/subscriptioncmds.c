/*-------------------------------------------------------------------------
 *
 * subscriptioncmds.c
 *		subscription catalog manipulation functions
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		subscriptioncmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/heapam.h"
#include "access/htup_details.h"

#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type.h"
#include "catalog/pg_subscription.h"

#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/subscriptioncmds.h"

#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"

#include "storage/lmgr.h"

#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/*
 * Common option parsing function for CREATE and ALTER SUBSCRIPTION commands.
 *
 * Since not all options can be specified in both commands, this function
 * will report an error on options if the target output pointer is NULL to
 * accomodate that.
 */
static void
parse_subscription_options(List *options, char **conninfo,
						   List **publications, bool *enabled_given,
						   bool *enabled, bool *create_slot, char **slot_name)
{
	ListCell   *lc;
	bool		create_slot_given = false;

	if (conninfo)
		*conninfo = NULL;
	if (publications)
		*publications = NIL;
	if (enabled)
	{
		*enabled_given = false;
		*enabled = true;
	}
	if (create_slot)
		*create_slot = true;
	if (slot_name)
		*slot_name = NULL;

	/* Parse options */
	foreach (lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "conninfo") == 0 && conninfo)
		{
			if (*conninfo)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*conninfo = defGetString(defel);
		}
		else if (strcmp(defel->defname, "publication") == 0 && publications)
		{
			if (*publications)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*publications = defGetStringList(defel);
		}
		else if (strcmp(defel->defname, "enabled") == 0 && enabled)
		{
			if (*enabled_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*enabled_given = true;
			*enabled = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "disabled") == 0 && enabled)
		{
			if (*enabled_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*enabled_given = true;
			*enabled = !defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "create slot") == 0 && create_slot)
		{
			if (create_slot_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			create_slot_given = true;
			*create_slot = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "nocreate slot") == 0 && create_slot)
		{
			if (create_slot_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			create_slot_given = true;
			*create_slot = !defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "slot name") == 0 && slot_name)
		{
			if (*slot_name)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*slot_name = defGetString(defel);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}
}

/*
 * Auxiliary function to return a text array out of a list of String nodes.
 */
static Datum
publicationListToArray(List *publist)
{
	ArrayType  *arr;
	Datum	   *datums;
	int			j = 0;
	ListCell   *cell;
	MemoryContext memcxt;
	MemoryContext oldcxt;

	/* Create memory context for temporary allocations. */
	memcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "publicationListToArray to array",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(memcxt);

	datums = palloc(sizeof(text *) * list_length(publist));
	foreach(cell, publist)
	{
		char	   *name = strVal(lfirst(cell));
		ListCell   *pcell;

		/* Check for duplicates. */
		foreach(pcell, publist)
		{
			char	   *pname = strVal(lfirst(cell));

			if (name == pname)
				break;

			if (strcmp(name, pname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("publication name \"%s\" used more than once",
								pname)));
		}

		datums[j++] = CStringGetTextDatum(name);
	}

	MemoryContextSwitchTo(oldcxt);

	arr = construct_array(datums, list_length(publist),
						  TEXTOID, -1, false, 'i');
	MemoryContextDelete(memcxt);

	return PointerGetDatum(arr);
}

/*
 * Create new subscription.
 */
ObjectAddress
CreateSubscription(CreateSubscriptionStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			subid;
	bool		nulls[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	Oid			owner = GetUserId();
	HeapTuple	tup;
	bool		enabled_given;
	bool		enabled;
	char	   *conninfo;
	char	   *slotname;
	char		originname[NAMEDATALEN];
	bool		create_slot;
	List	   *publications;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to create subscriptions"))));

	rel = heap_open(SubscriptionRelationId, RowExclusiveLock);

	/* Check if name is used */
	subid = GetSysCacheOid2(SUBSCRIPTIONNAME, MyDatabaseId,
							CStringGetDatum(stmt->subname));
	if (OidIsValid(subid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("subscription \"%s\" already exists",
						stmt->subname)));
	}

	/*
	 * Parse and check options.
	 * Connection and publication should not be specified here.
	 */
	parse_subscription_options(stmt->options, NULL, NULL,
							   &enabled_given, &enabled,
							   &create_slot, &slotname);
	if (slotname == NULL)
		slotname = stmt->subname;

	conninfo = stmt->conninfo;
	publications = stmt->publication;

	/* Load the library providing us libpq calls. */
	load_file("libpqwalreceiver", false);

	/* Check the connection info string. */
	walrcv_check_conninfo(conninfo);

	/* Everything ok, form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_subscription_subdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_subscription_subname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->subname));
	values[Anum_pg_subscription_subowner - 1] = ObjectIdGetDatum(owner);
	values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(enabled);
	values[Anum_pg_subscription_subconninfo - 1] =
		CStringGetTextDatum(conninfo);
	values[Anum_pg_subscription_subslotname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(slotname));
	values[Anum_pg_subscription_subpublications - 1] =
		 publicationListToArray(publications);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	subid = CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(SubscriptionRelationId, subid, owner);

	snprintf(originname, sizeof(originname), "pg_%u", subid);
	replorigin_create(originname);

	/*
	 * If requested, create the replication slot on remote side for our
	 * newly created subscription.
	 */
	if (create_slot)
	{
		XLogRecPtr			lsn;
		char			   *err;
		WalReceiverConn	   *wrconn;

		/* Try to connect to the publisher. */
		wrconn = walrcv_connect(conninfo, true, stmt->subname, &err);
		if (!wrconn)
			ereport(ERROR,
					(errmsg("could not connect to the publisher: %s", err)));

		PG_TRY();
		{
			walrcv_create_slot(wrconn, slotname, false, &lsn);
			ereport(NOTICE,
					(errmsg("created replication slot \"%s\" on publisher",
							slotname)));
		}
		PG_CATCH();
		{
			/* Close the connection in case of failure. */
			walrcv_disconnect(wrconn);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* And we are done with the remote side. */
		walrcv_disconnect(wrconn);
	}

	heap_close(rel, RowExclusiveLock);

	ApplyLauncherWakeupAtCommit();

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	InvokeObjectPostCreateHook(SubscriptionRelationId, subid, 0);

	return myself;
}

/*
 * Alter the existing subscription.
 */
ObjectAddress
AlterSubscription(AlterSubscriptionStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	HeapTuple	tup;
	Oid			subid;
	bool		enabled_given;
	bool		enabled;
	char	   *conninfo;
	char	   *slot_name;
	List	   *publications;

	rel = heap_open(SubscriptionRelationId, RowExclusiveLock);

	/* Fetch the existing tuple. */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(stmt->subname));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist",
						stmt->subname)));

	/* must be owner */
	if (!pg_subscription_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_SUBSCRIPTION,
					   stmt->subname);

	subid = HeapTupleGetOid(tup);

	/* Parse options. */
	parse_subscription_options(stmt->options, &conninfo, &publications,
							   &enabled_given, &enabled,
							   NULL, &slot_name);

	/* Form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (enabled_given)
	{
		values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(enabled);
		replaces[Anum_pg_subscription_subenabled - 1] = true;
	}
	if (conninfo)
	{
		values[Anum_pg_subscription_subconninfo - 1] =
			CStringGetTextDatum(conninfo);
		replaces[Anum_pg_subscription_subconninfo - 1] = true;
	}
	if (slot_name)
	{
		values[Anum_pg_subscription_subslotname - 1] =
			DirectFunctionCall1(namein, CStringGetDatum(slot_name));
		replaces[Anum_pg_subscription_subslotname - 1] = true;
	}
	if (publications != NIL)
	{
		values[Anum_pg_subscription_subpublications - 1] =
			 publicationListToArray(publications);
		replaces[Anum_pg_subscription_subpublications - 1] = true;
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);

	/* Update the catalog. */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	/* Cleanup. */
	heap_freetuple(tup);
	heap_close(rel, RowExclusiveLock);

	InvokeObjectPostAlterHook(SubscriptionRelationId, subid, 0);

	return myself;
}

/*
 * Drop a subscription
 */
void
DropSubscription(DropSubscriptionStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	HeapTuple	tup;
	Oid			subid;
	Datum		datum;
	bool		isnull;
	char	   *subname;
	char	   *conninfo;
	char	   *slotname;
	char		originname[NAMEDATALEN];
	char	   *err = NULL;
	RepOriginId	originid;
	WalReceiverConn	   *wrconn = NULL;
	StringInfoData		cmd;

	rel = heap_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCache2(SUBSCRIPTIONNAME, MyDatabaseId,
						  CStringGetDatum(stmt->subname));

	if (!HeapTupleIsValid(tup))
	{
		heap_close(rel, NoLock);

		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("subscription \"%s\" does not exist",
							stmt->subname)));
		else
			ereport(NOTICE,
					(errmsg("subscription \"%s\" does not exist, skipping",
							stmt->subname)));

		return;
	}

	subid = HeapTupleGetOid(tup);

	/* must be owner */
	if (!pg_subscription_ownercheck(subid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_SUBSCRIPTION,
					   stmt->subname);

	/* DROP hook for the subscription being removed */
	InvokeObjectDropHook(SubscriptionRelationId, subid, 0);

	/*
	 * Lock the subscription so nobody else can do anything with it
	 * (including the replication workers).
	 */
	LockSharedObject(SubscriptionRelationId, subid, 0, AccessExclusiveLock);

	/* Get subname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID, tup,
							Anum_pg_subscription_subname, &isnull);
	Assert(!isnull);
	subname = pstrdup(NameStr(*DatumGetName(datum)));

	/* Get conninfo */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID, tup,
							Anum_pg_subscription_subconninfo, &isnull);
	Assert(!isnull);
	conninfo = pstrdup(TextDatumGetCString(datum));

	/* Get slotname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID, tup,
							Anum_pg_subscription_subslotname, &isnull);
	Assert(!isnull);
	slotname = pstrdup(NameStr(*DatumGetName(datum)));

	ObjectAddressSet(myself, SubscriptionRelationId, subid);
	EventTriggerSQLDropAddObject(&myself, true, true);

	/* Remove the tuple from catalog. */
	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	/* Clean up dependencies */
	deleteSharedDependencyRecordsFor(SubscriptionRelationId, subid, 0);

	/* Protect against launcher restarting the worker. */
	LWLockAcquire(LogicalRepLauncherLock, LW_EXCLUSIVE);

	/* Kill the apply worker so that the slot becomes accessible. */
	logicalrep_worker_stop(subid);

	LWLockRelease(LogicalRepLauncherLock);

	/* Remove the origin tracking if exists. */
	snprintf(originname, sizeof(originname), "pg_%u", subid);
	originid = replorigin_by_name(originname, true);
	if (originid != InvalidRepOriginId)
		replorigin_drop(originid);

	/* If the user asked to not drop the slot, we are done mow.*/
	if (!stmt->drop_slot)
	{
		heap_close(rel, NoLock);
		return;
	}

	/*
	 * Otherwise drop the replication slot at the publisher node using
	 * the replication connection.
	 */
	load_file("libpqwalreceiver", false);

	initStringInfo(&cmd);
	appendStringInfo(&cmd, "DROP_REPLICATION_SLOT \"%s\"", slotname);

	wrconn = walrcv_connect(conninfo, true, subname, &err);
	if (wrconn == NULL)
		ereport(ERROR,
				(errmsg("could not connect to publisher when attempting to "
						"drop the replication slot \"%s\"", slotname),
				 errdetail("The error was: %s", err)));

	if (!walrcv_command(wrconn, cmd.data, &err))
	{
		/* Close the connection in case of failure */
		walrcv_disconnect(wrconn);
		ereport(ERROR,
				(errmsg("could not drop the replication slot \"%s\" on publisher",
						slotname),
				 errdetail("The error was: %s", err)));
	}
	else
		ereport(NOTICE,
				(errmsg("dropped replication slot \"%s\" on publisher",
						slotname)));

	walrcv_disconnect(wrconn);

	pfree(cmd.data);

	heap_close(rel, NoLock);
}

/*
 * Internal workhorse for changing a subscription owner
 */
static void
AlterSubscriptionOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_subscription form;

	form = (Form_pg_subscription) GETSTRUCT(tup);

	if (form->subowner == newOwnerId)
		return;

	if (!pg_subscription_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_SUBSCRIPTION,
					   NameStr(form->subname));

	/* New owner must be a superuser */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		  errmsg("permission denied to change owner of subscription \"%s\"",
				 NameStr(form->subname)),
			 errhint("The owner of an subscription must be a superuser.")));

	form->subowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Update owner dependency reference */
	changeDependencyOnOwner(SubscriptionRelationId,
							HeapTupleGetOid(tup),
							newOwnerId);

	InvokeObjectPostAlterHook(SubscriptionRelationId,
							  HeapTupleGetOid(tup), 0);
}

/*
 * Change subscription owner -- by name
 */
ObjectAddress
AlterSubscriptionOwner(const char *name, Oid newOwnerId)
{
	Oid			subid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;

	rel = heap_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist", name)));

	subid = HeapTupleGetOid(tup);

	AlterSubscriptionOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, SubscriptionRelationId, subid);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change subscription owner -- by OID
 */
void
AlterSubscriptionOwner_oid(Oid subid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription with OID %u does not exist", subid)));

	AlterSubscriptionOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);
}
