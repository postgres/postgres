/*-------------------------------------------------------------------------
 *
 * subscriptioncmds.c
 *		subscription catalog manipulation functions
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		subscriptioncmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"

#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"

#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/subscriptioncmds.h"

#include "executor/executor.h"

#include "nodes/makefuncs.h"

#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/worker_internal.h"

#include "storage/lmgr.h"

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static List *fetch_table_list(WalReceiverConn *wrconn, List *publications);

/*
 * Common option parsing function for CREATE and ALTER SUBSCRIPTION commands.
 *
 * Since not all options can be specified in both commands, this function
 * will report an error on options if the target output pointer is NULL to
 * accommodate that.
 */
static void
parse_subscription_options(List *options, bool *connect, bool *enabled_given,
						   bool *enabled, bool *create_slot,
						   bool *slot_name_given, char **slot_name,
						   bool *copy_data, char **synchronous_commit,
						   bool *refresh)
{
	ListCell   *lc;
	bool		connect_given = false;
	bool		create_slot_given = false;
	bool		copy_data_given = false;
	bool		refresh_given = false;

	/* If connect is specified, the others also need to be. */
	Assert(!connect || (enabled && create_slot && copy_data));

	if (connect)
		*connect = true;
	if (enabled)
	{
		*enabled_given = false;
		*enabled = true;
	}
	if (create_slot)
		*create_slot = true;
	if (slot_name)
	{
		*slot_name_given = false;
		*slot_name = NULL;
	}
	if (copy_data)
		*copy_data = true;
	if (synchronous_commit)
		*synchronous_commit = NULL;
	if (refresh)
		*refresh = true;

	/* Parse options */
	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "connect") == 0 && connect)
		{
			if (connect_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			connect_given = true;
			*connect = defGetBoolean(defel);
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
		else if (strcmp(defel->defname, "create_slot") == 0 && create_slot)
		{
			if (create_slot_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			create_slot_given = true;
			*create_slot = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "slot_name") == 0 && slot_name)
		{
			if (*slot_name_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*slot_name_given = true;
			*slot_name = defGetString(defel);

			/* Setting slot_name = NONE is treated as no slot name. */
			if (strcmp(*slot_name, "none") == 0)
				*slot_name = NULL;
			else
				ReplicationSlotValidateName(*slot_name, ERROR);
		}
		else if (strcmp(defel->defname, "copy_data") == 0 && copy_data)
		{
			if (copy_data_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			copy_data_given = true;
			*copy_data = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "synchronous_commit") == 0 &&
				 synchronous_commit)
		{
			if (*synchronous_commit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			*synchronous_commit = defGetString(defel);

			/* Test if the given value is valid for synchronous_commit GUC. */
			(void) set_config_option("synchronous_commit", *synchronous_commit,
									 PGC_BACKEND, PGC_S_TEST, GUC_ACTION_SET,
									 false, 0, false);
		}
		else if (strcmp(defel->defname, "refresh") == 0 && refresh)
		{
			if (refresh_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			refresh_given = true;
			*refresh = defGetBoolean(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized subscription parameter: \"%s\"", defel->defname)));
	}

	/*
	 * We've been explicitly asked to not connect, that requires some
	 * additional processing.
	 */
	if (connect && !*connect)
	{
		/* Check for incompatible options from the user. */
		if (enabled && *enabled_given && *enabled)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/*- translator: both %s are strings of the form "option = value" */
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "enabled = true")));

		if (create_slot && create_slot_given && *create_slot)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "create_slot = true")));

		if (copy_data && copy_data_given && *copy_data)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "copy_data = true")));

		/* Change the defaults of other options. */
		*enabled = false;
		*create_slot = false;
		*copy_data = false;
	}

	/*
	 * Do additional checking for disallowed combination when slot_name = NONE
	 * was used.
	 */
	if (slot_name && *slot_name_given && !*slot_name)
	{
		if (enabled && *enabled_given && *enabled)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/*- translator: both %s are strings of the form "option = value" */
					 errmsg("%s and %s are mutually exclusive options",
							"slot_name = NONE", "enabled = true")));

		if (create_slot && create_slot_given && *create_slot)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"slot_name = NONE", "create_slot = true")));

		if (enabled && !*enabled_given && *enabled)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/*- translator: both %s are strings of the form "option = value" */
					 errmsg("subscription with %s must also set %s",
							"slot_name = NONE", "enabled = false")));

		if (create_slot && !create_slot_given && *create_slot)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("subscription with %s must also set %s",
							"slot_name = NONE", "create_slot = false")));
	}
}

/*
 * Auxiliary function to build a text array out of a list of String nodes.
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
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(memcxt);

	datums = (Datum *) palloc(sizeof(Datum) * list_length(publist));

	foreach(cell, publist)
	{
		char	   *name = strVal(lfirst(cell));
		ListCell   *pcell;

		/* Check for duplicates. */
		foreach(pcell, publist)
		{
			char	   *pname = strVal(lfirst(pcell));

			if (pcell == cell)
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
CreateSubscription(CreateSubscriptionStmt *stmt, bool isTopLevel)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			subid;
	bool		nulls[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	Oid			owner = GetUserId();
	HeapTuple	tup;
	bool		connect;
	bool		enabled_given;
	bool		enabled;
	bool		copy_data;
	char	   *synchronous_commit;
	char	   *conninfo;
	char	   *slotname;
	bool		slotname_given;
	char		originname[NAMEDATALEN];
	bool		create_slot;
	List	   *publications;

	/*
	 * Parse and check options.
	 *
	 * Connection and publication should not be specified here.
	 */
	parse_subscription_options(stmt->options, &connect, &enabled_given,
							   &enabled, &create_slot, &slotname_given,
							   &slotname, &copy_data, &synchronous_commit,
							   NULL);

	/*
	 * Since creating a replication slot is not transactional, rolling back
	 * the transaction leaves the created replication slot.  So we cannot run
	 * CREATE SUBSCRIPTION inside a transaction block if creating a
	 * replication slot.
	 */
	if (create_slot)
		PreventInTransactionBlock(isTopLevel, "CREATE SUBSCRIPTION ... WITH (create_slot = true)");

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to create subscriptions"))));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for subscription names are violated.
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->subname, "regress_", 8) != 0)
		elog(WARNING, "subscriptions created by regression test cases should have names starting with \"regress_\"");
#endif

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* Check if name is used */
	subid = GetSysCacheOid2(SUBSCRIPTIONNAME, Anum_pg_subscription_oid,
							MyDatabaseId, CStringGetDatum(stmt->subname));
	if (OidIsValid(subid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("subscription \"%s\" already exists",
						stmt->subname)));
	}

	if (!slotname_given && slotname == NULL)
		slotname = stmt->subname;

	/* The default for synchronous_commit of subscriptions is off. */
	if (synchronous_commit == NULL)
		synchronous_commit = "off";

	conninfo = stmt->conninfo;
	publications = stmt->publication;

	/* Load the library providing us libpq calls. */
	load_file("libpqwalreceiver", false);

	/* Check the connection info string. */
	walrcv_check_conninfo(conninfo);

	/* Everything ok, form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	subid = GetNewOidWithIndex(rel, SubscriptionObjectIndexId,
							   Anum_pg_subscription_oid);
	values[Anum_pg_subscription_oid - 1] = ObjectIdGetDatum(subid);
	values[Anum_pg_subscription_subdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_subscription_subname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->subname));
	values[Anum_pg_subscription_subowner - 1] = ObjectIdGetDatum(owner);
	values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(enabled);
	values[Anum_pg_subscription_subconninfo - 1] =
		CStringGetTextDatum(conninfo);
	if (slotname)
		values[Anum_pg_subscription_subslotname - 1] =
			DirectFunctionCall1(namein, CStringGetDatum(slotname));
	else
		nulls[Anum_pg_subscription_subslotname - 1] = true;
	values[Anum_pg_subscription_subsynccommit - 1] =
		CStringGetTextDatum(synchronous_commit);
	values[Anum_pg_subscription_subpublications - 1] =
		publicationListToArray(publications);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(SubscriptionRelationId, subid, owner);

	snprintf(originname, sizeof(originname), "pg_%u", subid);
	replorigin_create(originname);

	/*
	 * Connect to remote side to execute requested commands and fetch table
	 * info.
	 */
	if (connect)
	{
		XLogRecPtr	lsn;
		char	   *err;
		WalReceiverConn *wrconn;
		List	   *tables;
		ListCell   *lc;
		char		table_state;

		/* Try to connect to the publisher. */
		wrconn = walrcv_connect(conninfo, true, stmt->subname, &err);
		if (!wrconn)
			ereport(ERROR,
					(errmsg("could not connect to the publisher: %s", err)));

		PG_TRY();
		{
			/*
			 * Set sync state based on if we were asked to do data copy or
			 * not.
			 */
			table_state = copy_data ? SUBREL_STATE_INIT : SUBREL_STATE_READY;

			/*
			 * Get the table list from publisher and build local table status
			 * info.
			 */
			tables = fetch_table_list(wrconn, publications);
			foreach(lc, tables)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);
				Oid			relid;

				relid = RangeVarGetRelid(rv, AccessShareLock, false);

				/* Check for supported relkind. */
				CheckSubscriptionRelkind(get_rel_relkind(relid),
										 rv->schemaname, rv->relname);

				AddSubscriptionRelState(subid, relid, table_state,
										InvalidXLogRecPtr);
			}

			/*
			 * If requested, create permanent slot for the subscription. We
			 * won't use the initial snapshot for anything, so no need to
			 * export it.
			 */
			if (create_slot)
			{
				Assert(slotname);

				walrcv_create_slot(wrconn, slotname, false,
								   CRS_NOEXPORT_SNAPSHOT, &lsn);
				ereport(NOTICE,
						(errmsg("created replication slot \"%s\" on publisher",
								slotname)));
			}
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
	else
		ereport(WARNING,
		/* translator: %s is an SQL ALTER statement */
				(errmsg("tables were not subscribed, you will have to run %s to subscribe the tables",
						"ALTER SUBSCRIPTION ... REFRESH PUBLICATION")));

	table_close(rel, RowExclusiveLock);

	if (enabled)
		ApplyLauncherWakeupAtCommit();

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	InvokeObjectPostCreateHook(SubscriptionRelationId, subid, 0);

	return myself;
}

static void
AlterSubscription_refresh(Subscription *sub, bool copy_data)
{
	char	   *err;
	List	   *pubrel_names;
	List	   *subrel_states;
	Oid		   *subrel_local_oids;
	Oid		   *pubrel_local_oids;
	WalReceiverConn *wrconn;
	ListCell   *lc;
	int			off;

	/* Load the library providing us libpq calls. */
	load_file("libpqwalreceiver", false);

	/* Try to connect to the publisher. */
	wrconn = walrcv_connect(sub->conninfo, true, sub->name, &err);
	if (!wrconn)
		ereport(ERROR,
				(errmsg("could not connect to the publisher: %s", err)));

	/* Get the table list from publisher. */
	pubrel_names = fetch_table_list(wrconn, sub->publications);

	/* We are done with the remote side, close connection. */
	walrcv_disconnect(wrconn);

	/* Get local table list. */
	subrel_states = GetSubscriptionRelations(sub->oid);

	/*
	 * Build qsorted array of local table oids for faster lookup. This can
	 * potentially contain all tables in the database so speed of lookup is
	 * important.
	 */
	subrel_local_oids = palloc(list_length(subrel_states) * sizeof(Oid));
	off = 0;
	foreach(lc, subrel_states)
	{
		SubscriptionRelState *relstate = (SubscriptionRelState *) lfirst(lc);

		subrel_local_oids[off++] = relstate->relid;
	}
	qsort(subrel_local_oids, list_length(subrel_states),
		  sizeof(Oid), oid_cmp);

	/*
	 * Walk over the remote tables and try to match them to locally known
	 * tables. If the table is not known locally create a new state for it.
	 *
	 * Also builds array of local oids of remote tables for the next step.
	 */
	off = 0;
	pubrel_local_oids = palloc(list_length(pubrel_names) * sizeof(Oid));

	foreach(lc, pubrel_names)
	{
		RangeVar   *rv = (RangeVar *) lfirst(lc);
		Oid			relid;

		relid = RangeVarGetRelid(rv, AccessShareLock, false);

		/* Check for supported relkind. */
		CheckSubscriptionRelkind(get_rel_relkind(relid),
								 rv->schemaname, rv->relname);

		pubrel_local_oids[off++] = relid;

		if (!bsearch(&relid, subrel_local_oids,
					 list_length(subrel_states), sizeof(Oid), oid_cmp))
		{
			AddSubscriptionRelState(sub->oid, relid,
									copy_data ? SUBREL_STATE_INIT : SUBREL_STATE_READY,
									InvalidXLogRecPtr);
			ereport(DEBUG1,
					(errmsg("table \"%s.%s\" added to subscription \"%s\"",
							rv->schemaname, rv->relname, sub->name)));
		}
	}

	/*
	 * Next remove state for tables we should not care about anymore using the
	 * data we collected above
	 */
	qsort(pubrel_local_oids, list_length(pubrel_names),
		  sizeof(Oid), oid_cmp);

	for (off = 0; off < list_length(subrel_states); off++)
	{
		Oid			relid = subrel_local_oids[off];

		if (!bsearch(&relid, pubrel_local_oids,
					 list_length(pubrel_names), sizeof(Oid), oid_cmp))
		{
			RemoveSubscriptionRel(sub->oid, relid);

			logicalrep_worker_stop_at_commit(sub->oid, relid);

			ereport(DEBUG1,
					(errmsg("table \"%s.%s\" removed from subscription \"%s\"",
							get_namespace_name(get_rel_namespace(relid)),
							get_rel_name(relid),
							sub->name)));
		}
	}
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
	bool		update_tuple = false;
	Subscription *sub;
	Form_pg_subscription form;

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* Fetch the existing tuple. */
	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(stmt->subname));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist",
						stmt->subname)));

	form = (Form_pg_subscription) GETSTRUCT(tup);
	subid = form->oid;

	/* must be owner */
	if (!pg_subscription_ownercheck(subid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SUBSCRIPTION,
					   stmt->subname);

	sub = GetSubscription(subid, false);

	/* Lock the subscription so nobody else can do anything with it. */
	LockSharedObject(SubscriptionRelationId, subid, 0, AccessExclusiveLock);

	/* Form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	switch (stmt->kind)
	{
		case ALTER_SUBSCRIPTION_OPTIONS:
			{
				char	   *slotname;
				bool		slotname_given;
				char	   *synchronous_commit;

				parse_subscription_options(stmt->options, NULL, NULL, NULL,
										   NULL, &slotname_given, &slotname,
										   NULL, &synchronous_commit, NULL);

				if (slotname_given)
				{
					if (sub->enabled && !slotname)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("cannot set %s for enabled subscription",
										"slot_name = NONE")));

					if (slotname)
						values[Anum_pg_subscription_subslotname - 1] =
							DirectFunctionCall1(namein, CStringGetDatum(slotname));
					else
						nulls[Anum_pg_subscription_subslotname - 1] = true;
					replaces[Anum_pg_subscription_subslotname - 1] = true;
				}

				if (synchronous_commit)
				{
					values[Anum_pg_subscription_subsynccommit - 1] =
						CStringGetTextDatum(synchronous_commit);
					replaces[Anum_pg_subscription_subsynccommit - 1] = true;
				}

				update_tuple = true;
				break;
			}

		case ALTER_SUBSCRIPTION_ENABLED:
			{
				bool		enabled,
							enabled_given;

				parse_subscription_options(stmt->options, NULL,
										   &enabled_given, &enabled, NULL,
										   NULL, NULL, NULL, NULL, NULL);
				Assert(enabled_given);

				if (!sub->slotname && enabled)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("cannot enable subscription that does not have a slot name")));

				values[Anum_pg_subscription_subenabled - 1] =
					BoolGetDatum(enabled);
				replaces[Anum_pg_subscription_subenabled - 1] = true;

				if (enabled)
					ApplyLauncherWakeupAtCommit();

				update_tuple = true;
				break;
			}

		case ALTER_SUBSCRIPTION_CONNECTION:
			/* Load the library providing us libpq calls. */
			load_file("libpqwalreceiver", false);
			/* Check the connection info string. */
			walrcv_check_conninfo(stmt->conninfo);

			values[Anum_pg_subscription_subconninfo - 1] =
				CStringGetTextDatum(stmt->conninfo);
			replaces[Anum_pg_subscription_subconninfo - 1] = true;
			update_tuple = true;
			break;

		case ALTER_SUBSCRIPTION_PUBLICATION:
			{
				bool		copy_data;
				bool		refresh;

				parse_subscription_options(stmt->options, NULL, NULL, NULL,
										   NULL, NULL, NULL, &copy_data,
										   NULL, &refresh);

				values[Anum_pg_subscription_subpublications - 1] =
					publicationListToArray(stmt->publication);
				replaces[Anum_pg_subscription_subpublications - 1] = true;

				update_tuple = true;

				/* Refresh if user asked us to. */
				if (refresh)
				{
					if (!sub->enabled)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("ALTER SUBSCRIPTION with refresh is not allowed for disabled subscriptions"),
								 errhint("Use ALTER SUBSCRIPTION ... SET PUBLICATION ... WITH (refresh = false).")));

					/* Make sure refresh sees the new list of publications. */
					sub->publications = stmt->publication;

					AlterSubscription_refresh(sub, copy_data);
				}

				break;
			}

		case ALTER_SUBSCRIPTION_REFRESH:
			{
				bool		copy_data;

				if (!sub->enabled)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("ALTER SUBSCRIPTION ... REFRESH is not allowed for disabled subscriptions")));

				parse_subscription_options(stmt->options, NULL, NULL, NULL,
										   NULL, NULL, NULL, &copy_data,
										   NULL, NULL);

				AlterSubscription_refresh(sub, copy_data);

				break;
			}

		default:
			elog(ERROR, "unrecognized ALTER SUBSCRIPTION kind %d",
				 stmt->kind);
	}

	/* Update the catalog if needed. */
	if (update_tuple)
	{
		tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								replaces);

		CatalogTupleUpdate(rel, &tup->t_self, tup);

		heap_freetuple(tup);
	}

	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	InvokeObjectPostAlterHook(SubscriptionRelationId, subid, 0);

	return myself;
}

/*
 * Drop a subscription
 */
void
DropSubscription(DropSubscriptionStmt *stmt, bool isTopLevel)
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
	List	   *subworkers;
	ListCell   *lc;
	char		originname[NAMEDATALEN];
	char	   *err = NULL;
	RepOriginId originid;
	WalReceiverConn *wrconn;
	StringInfoData cmd;
	Form_pg_subscription form;

	/*
	 * Lock pg_subscription with AccessExclusiveLock to ensure that the
	 * launcher doesn't restart new worker during dropping the subscription
	 */
	rel = table_open(SubscriptionRelationId, AccessExclusiveLock);

	tup = SearchSysCache2(SUBSCRIPTIONNAME, MyDatabaseId,
						  CStringGetDatum(stmt->subname));

	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, NoLock);

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

	form = (Form_pg_subscription) GETSTRUCT(tup);
	subid = form->oid;

	/* must be owner */
	if (!pg_subscription_ownercheck(subid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SUBSCRIPTION,
					   stmt->subname);

	/* DROP hook for the subscription being removed */
	InvokeObjectDropHook(SubscriptionRelationId, subid, 0);

	/*
	 * Lock the subscription so nobody else can do anything with it (including
	 * the replication workers).
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
	conninfo = TextDatumGetCString(datum);

	/* Get slotname */
	datum = SysCacheGetAttr(SUBSCRIPTIONOID, tup,
							Anum_pg_subscription_subslotname, &isnull);
	if (!isnull)
		slotname = pstrdup(NameStr(*DatumGetName(datum)));
	else
		slotname = NULL;

	/*
	 * Since dropping a replication slot is not transactional, the replication
	 * slot stays dropped even if the transaction rolls back.  So we cannot
	 * run DROP SUBSCRIPTION inside a transaction block if dropping the
	 * replication slot.
	 *
	 * XXX The command name should really be something like "DROP SUBSCRIPTION
	 * of a subscription that is associated with a replication slot", but we
	 * don't have the proper facilities for that.
	 */
	if (slotname)
		PreventInTransactionBlock(isTopLevel, "DROP SUBSCRIPTION");

	ObjectAddressSet(myself, SubscriptionRelationId, subid);
	EventTriggerSQLDropAddObject(&myself, true, true);

	/* Remove the tuple from catalog. */
	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	/*
	 * Stop all the subscription workers immediately.
	 *
	 * This is necessary if we are dropping the replication slot, so that the
	 * slot becomes accessible.
	 *
	 * It is also necessary if the subscription is disabled and was disabled
	 * in the same transaction.  Then the workers haven't seen the disabling
	 * yet and will still be running, leading to hangs later when we want to
	 * drop the replication origin.  If the subscription was disabled before
	 * this transaction, then there shouldn't be any workers left, so this
	 * won't make a difference.
	 *
	 * New workers won't be started because we hold an exclusive lock on the
	 * subscription till the end of the transaction.
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
	subworkers = logicalrep_workers_find(subid, false);
	LWLockRelease(LogicalRepWorkerLock);
	foreach(lc, subworkers)
	{
		LogicalRepWorker *w = (LogicalRepWorker *) lfirst(lc);

		logicalrep_worker_stop(w->subid, w->relid);
	}
	list_free(subworkers);

	/* Clean up dependencies */
	deleteSharedDependencyRecordsFor(SubscriptionRelationId, subid, 0);

	/* Remove any associated relation synchronization states. */
	RemoveSubscriptionRel(subid, InvalidOid);

	/* Remove the origin tracking if exists. */
	snprintf(originname, sizeof(originname), "pg_%u", subid);
	originid = replorigin_by_name(originname, true);
	if (originid != InvalidRepOriginId)
		replorigin_drop(originid, false);

	/*
	 * If there is no slot associated with the subscription, we can finish
	 * here.
	 */
	if (!slotname)
	{
		table_close(rel, NoLock);
		return;
	}

	/*
	 * Otherwise drop the replication slot at the publisher node using the
	 * replication connection.
	 */
	load_file("libpqwalreceiver", false);

	initStringInfo(&cmd);
	appendStringInfo(&cmd, "DROP_REPLICATION_SLOT %s WAIT", quote_identifier(slotname));

	wrconn = walrcv_connect(conninfo, true, subname, &err);
	if (wrconn == NULL)
		ereport(ERROR,
				(errmsg("could not connect to publisher when attempting to "
						"drop the replication slot \"%s\"", slotname),
				 errdetail("The error was: %s", err),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s to disable the subscription, and then use %s to disassociate it from the slot.",
						 "ALTER SUBSCRIPTION ... DISABLE",
						 "ALTER SUBSCRIPTION ... SET (slot_name = NONE)")));

	PG_TRY();
	{
		WalRcvExecResult *res;

		res = walrcv_exec(wrconn, cmd.data, 0, NULL);

		if (res->status != WALRCV_OK_COMMAND)
			ereport(ERROR,
					(errmsg("could not drop the replication slot \"%s\" on publisher",
							slotname),
					 errdetail("The error was: %s", res->err)));
		else
			ereport(NOTICE,
					(errmsg("dropped replication slot \"%s\" on publisher",
							slotname)));

		walrcv_clear_result(res);
	}
	PG_CATCH();
	{
		/* Close the connection in case of failure */
		walrcv_disconnect(wrconn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	walrcv_disconnect(wrconn);

	pfree(cmd.data);

	table_close(rel, NoLock);
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

	if (!pg_subscription_ownercheck(form->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SUBSCRIPTION,
					   NameStr(form->subname));

	/* New owner must be a superuser */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of subscription \"%s\"",
						NameStr(form->subname)),
				 errhint("The owner of a subscription must be a superuser.")));

	form->subowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Update owner dependency reference */
	changeDependencyOnOwner(SubscriptionRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(SubscriptionRelationId,
							  form->oid, 0);
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
	Form_pg_subscription form;

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(SUBSCRIPTIONNAME, MyDatabaseId,
							  CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription \"%s\" does not exist", name)));

	form = (Form_pg_subscription) GETSTRUCT(tup);
	subid = form->oid;

	AlterSubscriptionOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, SubscriptionRelationId, subid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

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

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("subscription with OID %u does not exist", subid)));

	AlterSubscriptionOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Get the list of tables which belong to specified publications on the
 * publisher connection.
 */
static List *
fetch_table_list(WalReceiverConn *wrconn, List *publications)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[2] = {TEXTOID, TEXTOID};
	ListCell   *lc;
	bool		first;
	List	   *tablelist = NIL;

	Assert(list_length(publications) > 0);

	initStringInfo(&cmd);
	appendStringInfoString(&cmd, "SELECT DISTINCT t.schemaname, t.tablename\n"
						   "  FROM pg_catalog.pg_publication_tables t\n"
						   " WHERE t.pubname IN (");
	first = true;
	foreach(lc, publications)
	{
		char	   *pubname = strVal(lfirst(lc));

		if (first)
			first = false;
		else
			appendStringInfoString(&cmd, ", ");

		appendStringInfoString(&cmd, quote_literal_cstr(pubname));
	}
	appendStringInfoChar(&cmd, ')');

	res = walrcv_exec(wrconn, cmd.data, 2, tableRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errmsg("could not receive list of replicated tables from the publisher: %s",
						res->err)));

	/* Process tables. */
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *nspname;
		char	   *relname;
		bool		isnull;
		RangeVar   *rv;

		nspname = TextDatumGetCString(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);
		relname = TextDatumGetCString(slot_getattr(slot, 2, &isnull));
		Assert(!isnull);

		rv = makeRangeVar(pstrdup(nspname), pstrdup(relname), -1);
		tablelist = lappend(tablelist, rv);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	walrcv_clear_result(res);

	return tablelist;
}
