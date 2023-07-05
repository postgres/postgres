/*-------------------------------------------------------------------------
 *
 * subscriptioncmds.c
 *		subscription catalog manipulation functions
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/commands/subscriptioncmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/subscriptioncmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/syscache.h"

/*
 * Options that can be specified by the user in CREATE/ALTER SUBSCRIPTION
 * command.
 */
#define SUBOPT_CONNECT				0x00000001
#define SUBOPT_ENABLED				0x00000002
#define SUBOPT_CREATE_SLOT			0x00000004
#define SUBOPT_SLOT_NAME			0x00000008
#define SUBOPT_COPY_DATA			0x00000010
#define SUBOPT_SYNCHRONOUS_COMMIT	0x00000020
#define SUBOPT_REFRESH				0x00000040
#define SUBOPT_BINARY				0x00000080
#define SUBOPT_STREAMING			0x00000100
#define SUBOPT_TWOPHASE_COMMIT		0x00000200
#define SUBOPT_DISABLE_ON_ERR		0x00000400
#define SUBOPT_LSN					0x00000800

/* check if the 'val' has 'bits' set */
#define IsSet(val, bits)  (((val) & (bits)) == (bits))

/*
 * Structure to hold a bitmap representing the user-provided CREATE/ALTER
 * SUBSCRIPTION command options and the parsed/default values of each of them.
 */
typedef struct SubOpts
{
	bits32		specified_opts;
	char	   *slot_name;
	char	   *synchronous_commit;
	bool		connect;
	bool		enabled;
	bool		create_slot;
	bool		copy_data;
	bool		refresh;
	bool		binary;
	bool		streaming;
	bool		twophase;
	bool		disableonerr;
	XLogRecPtr	lsn;
} SubOpts;

static List *fetch_table_list(WalReceiverConn *wrconn, List *publications);
static void check_duplicates_in_publist(List *publist, Datum *datums);
static List *merge_publications(List *oldpublist, List *newpublist, bool addpub, const char *subname);
static void ReportSlotConnectionError(List *rstates, Oid subid, char *slotname, char *err);


/*
 * Common option parsing function for CREATE and ALTER SUBSCRIPTION commands.
 *
 * Since not all options can be specified in both commands, this function
 * will report an error if mutually exclusive options are specified.
 */
static void
parse_subscription_options(ParseState *pstate, List *stmt_options,
						   bits32 supported_opts, SubOpts *opts)
{
	ListCell   *lc;

	/* Start out with cleared opts. */
	memset(opts, 0, sizeof(SubOpts));

	/* caller must expect some option */
	Assert(supported_opts != 0);

	/* If connect option is supported, these others also need to be. */
	Assert(!IsSet(supported_opts, SUBOPT_CONNECT) ||
		   IsSet(supported_opts, SUBOPT_ENABLED | SUBOPT_CREATE_SLOT |
				 SUBOPT_COPY_DATA));

	/* Set default values for the boolean supported options. */
	if (IsSet(supported_opts, SUBOPT_CONNECT))
		opts->connect = true;
	if (IsSet(supported_opts, SUBOPT_ENABLED))
		opts->enabled = true;
	if (IsSet(supported_opts, SUBOPT_CREATE_SLOT))
		opts->create_slot = true;
	if (IsSet(supported_opts, SUBOPT_COPY_DATA))
		opts->copy_data = true;
	if (IsSet(supported_opts, SUBOPT_REFRESH))
		opts->refresh = true;
	if (IsSet(supported_opts, SUBOPT_BINARY))
		opts->binary = false;
	if (IsSet(supported_opts, SUBOPT_STREAMING))
		opts->streaming = false;
	if (IsSet(supported_opts, SUBOPT_TWOPHASE_COMMIT))
		opts->twophase = false;
	if (IsSet(supported_opts, SUBOPT_DISABLE_ON_ERR))
		opts->disableonerr = false;

	/* Parse options */
	foreach(lc, stmt_options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (IsSet(supported_opts, SUBOPT_CONNECT) &&
			strcmp(defel->defname, "connect") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_CONNECT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_CONNECT;
			opts->connect = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_ENABLED) &&
				 strcmp(defel->defname, "enabled") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_ENABLED))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_ENABLED;
			opts->enabled = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_CREATE_SLOT) &&
				 strcmp(defel->defname, "create_slot") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_CREATE_SLOT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_CREATE_SLOT;
			opts->create_slot = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_SLOT_NAME) &&
				 strcmp(defel->defname, "slot_name") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_SLOT_NAME))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_SLOT_NAME;
			opts->slot_name = defGetString(defel);

			/* Setting slot_name = NONE is treated as no slot name. */
			if (strcmp(opts->slot_name, "none") == 0)
				opts->slot_name = NULL;
			else
				ReplicationSlotValidateName(opts->slot_name, ERROR);
		}
		else if (IsSet(supported_opts, SUBOPT_COPY_DATA) &&
				 strcmp(defel->defname, "copy_data") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_COPY_DATA))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_COPY_DATA;
			opts->copy_data = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_SYNCHRONOUS_COMMIT) &&
				 strcmp(defel->defname, "synchronous_commit") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_SYNCHRONOUS_COMMIT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_SYNCHRONOUS_COMMIT;
			opts->synchronous_commit = defGetString(defel);

			/* Test if the given value is valid for synchronous_commit GUC. */
			(void) set_config_option("synchronous_commit", opts->synchronous_commit,
									 PGC_BACKEND, PGC_S_TEST, GUC_ACTION_SET,
									 false, 0, false);
		}
		else if (IsSet(supported_opts, SUBOPT_REFRESH) &&
				 strcmp(defel->defname, "refresh") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_REFRESH))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_REFRESH;
			opts->refresh = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_BINARY) &&
				 strcmp(defel->defname, "binary") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_BINARY))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_BINARY;
			opts->binary = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_STREAMING) &&
				 strcmp(defel->defname, "streaming") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_STREAMING))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_STREAMING;
			opts->streaming = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			/*
			 * Do not allow toggling of two_phase option. Doing so could cause
			 * missing of transactions and lead to an inconsistent replica.
			 * See comments atop worker.c
			 *
			 * Note: Unsupported twophase indicates that this call originated
			 * from AlterSubscription.
			 */
			if (!IsSet(supported_opts, SUBOPT_TWOPHASE_COMMIT))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized subscription parameter: \"%s\"", defel->defname)));

			if (IsSet(opts->specified_opts, SUBOPT_TWOPHASE_COMMIT))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_TWOPHASE_COMMIT;
			opts->twophase = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_DISABLE_ON_ERR) &&
				 strcmp(defel->defname, "disable_on_error") == 0)
		{
			if (IsSet(opts->specified_opts, SUBOPT_DISABLE_ON_ERR))
				errorConflictingDefElem(defel, pstate);

			opts->specified_opts |= SUBOPT_DISABLE_ON_ERR;
			opts->disableonerr = defGetBoolean(defel);
		}
		else if (IsSet(supported_opts, SUBOPT_LSN) &&
				 strcmp(defel->defname, "lsn") == 0)
		{
			char	   *lsn_str = defGetString(defel);
			XLogRecPtr	lsn;

			if (IsSet(opts->specified_opts, SUBOPT_LSN))
				errorConflictingDefElem(defel, pstate);

			/* Setting lsn = NONE is treated as resetting LSN */
			if (strcmp(lsn_str, "none") == 0)
				lsn = InvalidXLogRecPtr;
			else
			{
				/* Parse the argument as LSN */
				lsn = DatumGetLSN(DirectFunctionCall1(pg_lsn_in,
													  CStringGetDatum(lsn_str)));

				if (XLogRecPtrIsInvalid(lsn))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid WAL location (LSN): %s", lsn_str)));
			}

			opts->specified_opts |= SUBOPT_LSN;
			opts->lsn = lsn;
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
	if (!opts->connect && IsSet(supported_opts, SUBOPT_CONNECT))
	{
		/* Check for incompatible options from the user. */
		if (opts->enabled &&
			IsSet(opts->specified_opts, SUBOPT_ENABLED))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			/*- translator: both %s are strings of the form "option = value" */
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "enabled = true")));

		if (opts->create_slot &&
			IsSet(opts->specified_opts, SUBOPT_CREATE_SLOT))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "create_slot = true")));

		if (opts->copy_data &&
			IsSet(opts->specified_opts, SUBOPT_COPY_DATA))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s and %s are mutually exclusive options",
							"connect = false", "copy_data = true")));

		/* Change the defaults of other options. */
		opts->enabled = false;
		opts->create_slot = false;
		opts->copy_data = false;
	}

	/*
	 * Do additional checking for disallowed combination when slot_name = NONE
	 * was used.
	 */
	if (!opts->slot_name &&
		IsSet(opts->specified_opts, SUBOPT_SLOT_NAME))
	{
		if (opts->enabled)
		{
			if (IsSet(opts->specified_opts, SUBOPT_ENABLED))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("%s and %s are mutually exclusive options",
								"slot_name = NONE", "enabled = true")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("subscription with %s must also set %s",
								"slot_name = NONE", "enabled = false")));
		}

		if (opts->create_slot)
		{
			if (IsSet(opts->specified_opts, SUBOPT_CREATE_SLOT))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("%s and %s are mutually exclusive options",
								"slot_name = NONE", "create_slot = true")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*- translator: both %s are strings of the form "option = value" */
						 errmsg("subscription with %s must also set %s",
								"slot_name = NONE", "create_slot = false")));
		}
	}
}

/*
 * Add publication names from the list to a string.
 */
static void
get_publications_str(List *publications, StringInfo dest, bool quote_literal)
{
	ListCell   *lc;
	bool		first = true;

	Assert(list_length(publications) > 0);

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
 * Check that the specified publications are present on the publisher.
 */
static void
check_publications(WalReceiverConn *wrconn, List *publications)
{
	WalRcvExecResult *res;
	StringInfo	cmd;
	TupleTableSlot *slot;
	List	   *publicationsCopy = NIL;
	Oid			tableRow[1] = {TEXTOID};

	cmd = makeStringInfo();
	appendStringInfoString(cmd, "SELECT t.pubname FROM\n"
						   " pg_catalog.pg_publication t WHERE\n"
						   " t.pubname IN (");
	get_publications_str(publications, cmd, true);
	appendStringInfoChar(cmd, ')');

	res = walrcv_exec(wrconn, cmd->data, 1, tableRow);
	pfree(cmd->data);
	pfree(cmd);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not receive list of publications from the publisher: %s",
					   res->err));

	publicationsCopy = list_copy(publications);

	/* Process publication(s). */
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *pubname;
		bool		isnull;

		pubname = TextDatumGetCString(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);

		/* Delete the publication present in publisher from the list. */
		publicationsCopy = list_delete(publicationsCopy, makeString(pubname));
		ExecClearTuple(slot);
	}

	ExecDropSingleTupleTableSlot(slot);

	walrcv_clear_result(res);

	if (list_length(publicationsCopy))
	{
		/* Prepare the list of non-existent publication(s) for error message. */
		StringInfo	pubnames = makeStringInfo();

		get_publications_str(publicationsCopy, pubnames, false);
		ereport(WARNING,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg_plural("publication %s does not exist on the publisher",
							  "publications %s do not exist on the publisher",
							  list_length(publicationsCopy),
							  pubnames->data));
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
	MemoryContext memcxt;
	MemoryContext oldcxt;

	/* Create memory context for temporary allocations. */
	memcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "publicationListToArray to array",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(memcxt);

	datums = (Datum *) palloc(sizeof(Datum) * list_length(publist));

	check_duplicates_in_publist(publist, datums);

	MemoryContextSwitchTo(oldcxt);

	arr = construct_array(datums, list_length(publist),
						  TEXTOID, -1, false, TYPALIGN_INT);

	MemoryContextDelete(memcxt);

	return PointerGetDatum(arr);
}

/*
 * Create new subscription.
 */
ObjectAddress
CreateSubscription(ParseState *pstate, CreateSubscriptionStmt *stmt,
				   bool isTopLevel)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			subid;
	bool		nulls[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];
	Oid			owner = GetUserId();
	HeapTuple	tup;
	char	   *conninfo;
	char		originname[NAMEDATALEN];
	List	   *publications;
	bits32		supported_opts;
	SubOpts		opts = {0};

	/*
	 * Parse and check options.
	 *
	 * Connection and publication should not be specified here.
	 */
	supported_opts = (SUBOPT_CONNECT | SUBOPT_ENABLED | SUBOPT_CREATE_SLOT |
					  SUBOPT_SLOT_NAME | SUBOPT_COPY_DATA |
					  SUBOPT_SYNCHRONOUS_COMMIT | SUBOPT_BINARY |
					  SUBOPT_STREAMING | SUBOPT_TWOPHASE_COMMIT |
					  SUBOPT_DISABLE_ON_ERR);
	parse_subscription_options(pstate, stmt->options, supported_opts, &opts);

	/*
	 * Since creating a replication slot is not transactional, rolling back
	 * the transaction leaves the created replication slot.  So we cannot run
	 * CREATE SUBSCRIPTION inside a transaction block if creating a
	 * replication slot.
	 */
	if (opts.create_slot)
		PreventInTransactionBlock(isTopLevel, "CREATE SUBSCRIPTION ... WITH (create_slot = true)");

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create subscriptions")));

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

	if (!IsSet(opts.specified_opts, SUBOPT_SLOT_NAME) &&
		opts.slot_name == NULL)
		opts.slot_name = stmt->subname;

	/* The default for synchronous_commit of subscriptions is off. */
	if (opts.synchronous_commit == NULL)
		opts.synchronous_commit = "off";

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
	values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(InvalidXLogRecPtr);
	values[Anum_pg_subscription_subname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->subname));
	values[Anum_pg_subscription_subowner - 1] = ObjectIdGetDatum(owner);
	values[Anum_pg_subscription_subenabled - 1] = BoolGetDatum(opts.enabled);
	values[Anum_pg_subscription_subbinary - 1] = BoolGetDatum(opts.binary);
	values[Anum_pg_subscription_substream - 1] = BoolGetDatum(opts.streaming);
	values[Anum_pg_subscription_subtwophasestate - 1] =
		CharGetDatum(opts.twophase ?
					 LOGICALREP_TWOPHASE_STATE_PENDING :
					 LOGICALREP_TWOPHASE_STATE_DISABLED);
	values[Anum_pg_subscription_subdisableonerr - 1] = BoolGetDatum(opts.disableonerr);
	values[Anum_pg_subscription_subconninfo - 1] =
		CStringGetTextDatum(conninfo);
	if (opts.slot_name)
		values[Anum_pg_subscription_subslotname - 1] =
			DirectFunctionCall1(namein, CStringGetDatum(opts.slot_name));
	else
		nulls[Anum_pg_subscription_subslotname - 1] = true;
	values[Anum_pg_subscription_subsynccommit - 1] =
		CStringGetTextDatum(opts.synchronous_commit);
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
	if (opts.connect)
	{
		char	   *err;
		WalReceiverConn *wrconn;
		List	   *tables;
		ListCell   *lc;
		char		table_state;

		/* Try to connect to the publisher. */
		wrconn = walrcv_connect(conninfo, true, stmt->subname, &err);
		if (!wrconn)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to the publisher: %s", err)));

		PG_TRY();
		{
			check_publications(wrconn, publications);

			/*
			 * Set sync state based on if we were asked to do data copy or
			 * not.
			 */
			table_state = opts.copy_data ? SUBREL_STATE_INIT : SUBREL_STATE_READY;

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
			if (opts.create_slot)
			{
				bool		twophase_enabled = false;

				Assert(opts.slot_name);

				/*
				 * Even if two_phase is set, don't create the slot with
				 * two-phase enabled. Will enable it once all the tables are
				 * synced and ready. This avoids race-conditions like prepared
				 * transactions being skipped due to changes not being applied
				 * due to checks in should_apply_changes_for_rel() when
				 * tablesync for the corresponding tables are in progress. See
				 * comments atop worker.c.
				 *
				 * Note that if tables were specified but copy_data is false
				 * then it is safe to enable two_phase up-front because those
				 * tables are already initially in READY state. When the
				 * subscription has no tables, we leave the twophase state as
				 * PENDING, to allow ALTER SUBSCRIPTION ... REFRESH
				 * PUBLICATION to work.
				 */
				if (opts.twophase && !opts.copy_data && tables != NIL)
					twophase_enabled = true;

				walrcv_create_slot(wrconn, opts.slot_name, false, twophase_enabled,
								   CRS_NOEXPORT_SNAPSHOT, NULL);

				if (twophase_enabled)
					UpdateTwoPhaseState(subid, LOGICALREP_TWOPHASE_STATE_ENABLED);

				ereport(NOTICE,
						(errmsg("created replication slot \"%s\" on publisher",
								opts.slot_name)));
			}
		}
		PG_FINALLY();
		{
			walrcv_disconnect(wrconn);
		}
		PG_END_TRY();
	}
	else
		ereport(WARNING,
		/* translator: %s is an SQL ALTER statement */
				(errmsg("tables were not subscribed, you will have to run %s to subscribe the tables",
						"ALTER SUBSCRIPTION ... REFRESH PUBLICATION")));

	table_close(rel, RowExclusiveLock);

	pgstat_create_subscription(subid);

	if (opts.enabled)
		ApplyLauncherWakeupAtCommit();

	ObjectAddressSet(myself, SubscriptionRelationId, subid);

	InvokeObjectPostCreateHook(SubscriptionRelationId, subid, 0);

	return myself;
}

static void
AlterSubscription_refresh(Subscription *sub, bool copy_data,
						  List *validate_publications)
{
	char	   *err;
	List	   *pubrel_names;
	List	   *subrel_states;
	Oid		   *subrel_local_oids;
	Oid		   *pubrel_local_oids;
	ListCell   *lc;
	int			off;
	int			remove_rel_len;
	Relation	rel = NULL;
	typedef struct SubRemoveRels
	{
		Oid			relid;
		char		state;
	} SubRemoveRels;
	SubRemoveRels *sub_remove_rels;
	WalReceiverConn *wrconn;

	/* Load the library providing us libpq calls. */
	load_file("libpqwalreceiver", false);

	/* Try to connect to the publisher. */
	wrconn = walrcv_connect(sub->conninfo, true, sub->name, &err);
	if (!wrconn)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to the publisher: %s", err)));

	PG_TRY();
	{
		if (validate_publications)
			check_publications(wrconn, validate_publications);

		/* Get the table list from publisher. */
		pubrel_names = fetch_table_list(wrconn, sub->publications);

		/* Get local table list. */
		subrel_states = GetSubscriptionRelations(sub->oid);

		/*
		 * Build qsorted array of local table oids for faster lookup. This can
		 * potentially contain all tables in the database so speed of lookup
		 * is important.
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
		 * Rels that we want to remove from subscription and drop any slots
		 * and origins corresponding to them.
		 */
		sub_remove_rels = palloc(list_length(subrel_states) * sizeof(SubRemoveRels));

		/*
		 * Walk over the remote tables and try to match them to locally known
		 * tables. If the table is not known locally create a new state for
		 * it.
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
						(errmsg_internal("table \"%s.%s\" added to subscription \"%s\"",
										 rv->schemaname, rv->relname, sub->name)));
			}
		}

		/*
		 * Next remove state for tables we should not care about anymore using
		 * the data we collected above
		 */
		qsort(pubrel_local_oids, list_length(pubrel_names),
			  sizeof(Oid), oid_cmp);

		remove_rel_len = 0;
		for (off = 0; off < list_length(subrel_states); off++)
		{
			Oid			relid = subrel_local_oids[off];

			if (!bsearch(&relid, pubrel_local_oids,
						 list_length(pubrel_names), sizeof(Oid), oid_cmp))
			{
				char		state;
				XLogRecPtr	statelsn;

				/*
				 * Lock pg_subscription_rel with AccessExclusiveLock to
				 * prevent any race conditions with the apply worker
				 * re-launching workers at the same time this code is trying
				 * to remove those tables.
				 *
				 * Even if new worker for this particular rel is restarted it
				 * won't be able to make any progress as we hold exclusive
				 * lock on subscription_rel till the transaction end. It will
				 * simply exit as there is no corresponding rel entry.
				 *
				 * This locking also ensures that the state of rels won't
				 * change till we are done with this refresh operation.
				 */
				if (!rel)
					rel = table_open(SubscriptionRelRelationId, AccessExclusiveLock);

				/* Last known rel state. */
				state = GetSubscriptionRelState(sub->oid, relid, &statelsn);

				sub_remove_rels[remove_rel_len].relid = relid;
				sub_remove_rels[remove_rel_len++].state = state;

				RemoveSubscriptionRel(sub->oid, relid);

				logicalrep_worker_stop(sub->oid, relid);

				/*
				 * For READY state, we would have already dropped the
				 * tablesync origin.
				 */
				if (state != SUBREL_STATE_READY)
				{
					char		originname[NAMEDATALEN];

					/*
					 * Drop the tablesync's origin tracking if exists.
					 *
					 * It is possible that the origin is not yet created for
					 * tablesync worker, this can happen for the states before
					 * SUBREL_STATE_FINISHEDCOPY. The apply worker can also
					 * concurrently try to drop the origin and by this time
					 * the origin might be already removed. For these reasons,
					 * passing missing_ok = true.
					 */
					ReplicationOriginNameForTablesync(sub->oid, relid, originname,
													  sizeof(originname));
					replorigin_drop_by_name(originname, true, false);
				}

				ereport(DEBUG1,
						(errmsg_internal("table \"%s.%s\" removed from subscription \"%s\"",
										 get_namespace_name(get_rel_namespace(relid)),
										 get_rel_name(relid),
										 sub->name)));
			}
		}

		/*
		 * Drop the tablesync slots associated with removed tables. This has
		 * to be at the end because otherwise if there is an error while doing
		 * the database operations we won't be able to rollback dropped slots.
		 */
		for (off = 0; off < remove_rel_len; off++)
		{
			if (sub_remove_rels[off].state != SUBREL_STATE_READY &&
				sub_remove_rels[off].state != SUBREL_STATE_SYNCDONE)
			{
				char		syncslotname[NAMEDATALEN] = {0};

				/*
				 * For READY/SYNCDONE states we know the tablesync slot has
				 * already been dropped by the tablesync worker.
				 *
				 * For other states, there is no certainty, maybe the slot
				 * does not exist yet. Also, if we fail after removing some of
				 * the slots, next time, it will again try to drop already
				 * dropped slots and fail. For these reasons, we allow
				 * missing_ok = true for the drop.
				 */
				ReplicationSlotNameForTablesync(sub->oid, sub_remove_rels[off].relid,
												syncslotname, sizeof(syncslotname));
				ReplicationSlotDropAtPubNode(wrconn, syncslotname, true);
			}
		}
	}
	PG_FINALLY();
	{
		walrcv_disconnect(wrconn);
	}
	PG_END_TRY();

	if (rel)
		table_close(rel, NoLock);
}

/*
 * Alter the existing subscription.
 */
ObjectAddress
AlterSubscription(ParseState *pstate, AlterSubscriptionStmt *stmt,
				  bool isTopLevel)
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
	bits32		supported_opts;
	SubOpts		opts = {0};

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
				supported_opts = (SUBOPT_SLOT_NAME |
								  SUBOPT_SYNCHRONOUS_COMMIT | SUBOPT_BINARY |
								  SUBOPT_STREAMING | SUBOPT_DISABLE_ON_ERR);

				parse_subscription_options(pstate, stmt->options,
										   supported_opts, &opts);

				if (IsSet(opts.specified_opts, SUBOPT_SLOT_NAME))
				{
					/*
					 * The subscription must be disabled to allow slot_name as
					 * 'none', otherwise, the apply worker will repeatedly try
					 * to stream the data using that slot_name which neither
					 * exists on the publisher nor the user will be allowed to
					 * create it.
					 */
					if (sub->enabled && !opts.slot_name)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot set %s for enabled subscription",
										"slot_name = NONE")));

					if (opts.slot_name)
						values[Anum_pg_subscription_subslotname - 1] =
							DirectFunctionCall1(namein, CStringGetDatum(opts.slot_name));
					else
						nulls[Anum_pg_subscription_subslotname - 1] = true;
					replaces[Anum_pg_subscription_subslotname - 1] = true;
				}

				if (opts.synchronous_commit)
				{
					values[Anum_pg_subscription_subsynccommit - 1] =
						CStringGetTextDatum(opts.synchronous_commit);
					replaces[Anum_pg_subscription_subsynccommit - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_BINARY))
				{
					values[Anum_pg_subscription_subbinary - 1] =
						BoolGetDatum(opts.binary);
					replaces[Anum_pg_subscription_subbinary - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_STREAMING))
				{
					values[Anum_pg_subscription_substream - 1] =
						BoolGetDatum(opts.streaming);
					replaces[Anum_pg_subscription_substream - 1] = true;
				}

				if (IsSet(opts.specified_opts, SUBOPT_DISABLE_ON_ERR))
				{
					values[Anum_pg_subscription_subdisableonerr - 1]
						= BoolGetDatum(opts.disableonerr);
					replaces[Anum_pg_subscription_subdisableonerr - 1]
						= true;
				}

				update_tuple = true;
				break;
			}

		case ALTER_SUBSCRIPTION_ENABLED:
			{
				parse_subscription_options(pstate, stmt->options,
										   SUBOPT_ENABLED, &opts);
				Assert(IsSet(opts.specified_opts, SUBOPT_ENABLED));

				if (!sub->slotname && opts.enabled)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("cannot enable subscription that does not have a slot name")));

				values[Anum_pg_subscription_subenabled - 1] =
					BoolGetDatum(opts.enabled);
				replaces[Anum_pg_subscription_subenabled - 1] = true;

				if (opts.enabled)
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

		case ALTER_SUBSCRIPTION_SET_PUBLICATION:
			{
				supported_opts = SUBOPT_COPY_DATA | SUBOPT_REFRESH;
				parse_subscription_options(pstate, stmt->options,
										   supported_opts, &opts);

				values[Anum_pg_subscription_subpublications - 1] =
					publicationListToArray(stmt->publication);
				replaces[Anum_pg_subscription_subpublications - 1] = true;

				update_tuple = true;

				/* Refresh if user asked us to. */
				if (opts.refresh)
				{
					if (!sub->enabled)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh is not allowed for disabled subscriptions"),
								 errhint("Use ALTER SUBSCRIPTION ... SET PUBLICATION ... WITH (refresh = false).")));

					/*
					 * See ALTER_SUBSCRIPTION_REFRESH for details why this is
					 * not allowed.
					 */
					if (sub->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED && opts.copy_data)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh and copy_data is not allowed when two_phase is enabled"),
								 errhint("Use ALTER SUBSCRIPTION ... SET PUBLICATION with refresh = false, or with copy_data = false, or use DROP/CREATE SUBSCRIPTION.")));

					PreventInTransactionBlock(isTopLevel, "ALTER SUBSCRIPTION with refresh");

					/* Make sure refresh sees the new list of publications. */
					sub->publications = stmt->publication;

					AlterSubscription_refresh(sub, opts.copy_data,
											  stmt->publication);
				}

				break;
			}

		case ALTER_SUBSCRIPTION_ADD_PUBLICATION:
		case ALTER_SUBSCRIPTION_DROP_PUBLICATION:
			{
				List	   *publist;
				bool		isadd = stmt->kind == ALTER_SUBSCRIPTION_ADD_PUBLICATION;

				supported_opts = SUBOPT_REFRESH | SUBOPT_COPY_DATA;
				parse_subscription_options(pstate, stmt->options,
										   supported_opts, &opts);

				publist = merge_publications(sub->publications, stmt->publication, isadd, stmt->subname);
				values[Anum_pg_subscription_subpublications - 1] =
					publicationListToArray(publist);
				replaces[Anum_pg_subscription_subpublications - 1] = true;

				update_tuple = true;

				/* Refresh if user asked us to. */
				if (opts.refresh)
				{
					/* We only need to validate user specified publications. */
					List	   *validate_publications = (isadd) ? stmt->publication : NULL;

					if (!sub->enabled)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh is not allowed for disabled subscriptions"),
						/* translator: %s is an SQL ALTER command */
								 errhint("Use %s instead.",
										 isadd ?
										 "ALTER SUBSCRIPTION ... ADD PUBLICATION ... WITH (refresh = false)" :
										 "ALTER SUBSCRIPTION ... DROP PUBLICATION ... WITH (refresh = false)")));

					/*
					 * See ALTER_SUBSCRIPTION_REFRESH for details why this is
					 * not allowed.
					 */
					if (sub->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED && opts.copy_data)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("ALTER SUBSCRIPTION with refresh and copy_data is not allowed when two_phase is enabled"),
						/* translator: %s is an SQL ALTER command */
								 errhint("Use %s with refresh = false, or with copy_data = false, or use DROP/CREATE SUBSCRIPTION.",
										 isadd ?
										 "ALTER SUBSCRIPTION ... ADD PUBLICATION" :
										 "ALTER SUBSCRIPTION ... DROP PUBLICATION")));

					PreventInTransactionBlock(isTopLevel, "ALTER SUBSCRIPTION with refresh");

					/* Refresh the new list of publications. */
					sub->publications = publist;

					AlterSubscription_refresh(sub, opts.copy_data,
											  validate_publications);
				}

				break;
			}

		case ALTER_SUBSCRIPTION_REFRESH:
			{
				if (!sub->enabled)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("ALTER SUBSCRIPTION ... REFRESH is not allowed for disabled subscriptions")));

				parse_subscription_options(pstate, stmt->options,
										   SUBOPT_COPY_DATA, &opts);

				/*
				 * The subscription option "two_phase" requires that
				 * replication has passed the initial table synchronization
				 * phase before the two_phase becomes properly enabled.
				 *
				 * But, having reached this two-phase commit "enabled" state
				 * we must not allow any subsequent table initialization to
				 * occur. So the ALTER SUBSCRIPTION ... REFRESH is disallowed
				 * when the user had requested two_phase = on mode.
				 *
				 * The exception to this restriction is when copy_data =
				 * false, because when copy_data is false the tablesync will
				 * start already in READY state and will exit directly without
				 * doing anything.
				 *
				 * For more details see comments atop worker.c.
				 */
				if (sub->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED && opts.copy_data)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("ALTER SUBSCRIPTION ... REFRESH with copy_data is not allowed when two_phase is enabled"),
							 errhint("Use ALTER SUBSCRIPTION ... REFRESH with copy_data = false, or use DROP/CREATE SUBSCRIPTION.")));

				PreventInTransactionBlock(isTopLevel, "ALTER SUBSCRIPTION ... REFRESH");

				AlterSubscription_refresh(sub, opts.copy_data, NULL);

				break;
			}

		case ALTER_SUBSCRIPTION_SKIP:
			{
				parse_subscription_options(pstate, stmt->options, SUBOPT_LSN, &opts);

				/* ALTER SUBSCRIPTION ... SKIP supports only LSN option */
				Assert(IsSet(opts.specified_opts, SUBOPT_LSN));

				if (!superuser())
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("must be superuser to skip transaction")));

				/*
				 * If the user sets subskiplsn, we do a sanity check to make
				 * sure that the specified LSN is a probable value.
				 */
				if (!XLogRecPtrIsInvalid(opts.lsn))
				{
					RepOriginId originid;
					char		originname[NAMEDATALEN];
					XLogRecPtr	remote_lsn;

					snprintf(originname, sizeof(originname), "pg_%u", subid);
					originid = replorigin_by_name(originname, false);
					remote_lsn = replorigin_get_progress(originid, false);

					/* Check the given LSN is at least a future LSN */
					if (!XLogRecPtrIsInvalid(remote_lsn) && opts.lsn < remote_lsn)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("skip WAL location (LSN %X/%X) must be greater than origin LSN %X/%X",
										LSN_FORMAT_ARGS(opts.lsn),
										LSN_FORMAT_ARGS(remote_lsn))));
				}

				values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(opts.lsn);
				replaces[Anum_pg_subscription_subskiplsn - 1] = true;

				update_tuple = true;
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
	WalReceiverConn *wrconn;
	Form_pg_subscription form;
	List	   *rstates;

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
	 * replication slot.  Also, in this case, we report a message for dropping
	 * the subscription to the cumulative stats system.
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

	/*
	 * Cleanup of tablesync replication origins.
	 *
	 * Any READY-state relations would already have dealt with clean-ups.
	 *
	 * Note that the state can't change because we have already stopped both
	 * the apply and tablesync workers and they can't restart because of
	 * exclusive lock on the subscription.
	 */
	rstates = GetSubscriptionNotReadyRelations(subid);
	foreach(lc, rstates)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);
		Oid			relid = rstate->relid;

		/* Only cleanup resources of tablesync workers */
		if (!OidIsValid(relid))
			continue;

		/*
		 * Drop the tablesync's origin tracking if exists.
		 *
		 * It is possible that the origin is not yet created for tablesync
		 * worker so passing missing_ok = true. This can happen for the states
		 * before SUBREL_STATE_FINISHEDCOPY.
		 */
		ReplicationOriginNameForTablesync(subid, relid, originname,
										  sizeof(originname));
		replorigin_drop_by_name(originname, true, false);
	}

	/* Clean up dependencies */
	deleteSharedDependencyRecordsFor(SubscriptionRelationId, subid, 0);

	/* Remove any associated relation synchronization states. */
	RemoveSubscriptionRel(subid, InvalidOid);

	/* Remove the origin tracking if exists. */
	snprintf(originname, sizeof(originname), "pg_%u", subid);
	replorigin_drop_by_name(originname, true, false);

	/*
	 * Tell the cumulative stats system that the subscription is getting
	 * dropped.
	 */
	pgstat_drop_subscription(subid);

	/*
	 * If there is no slot associated with the subscription, we can finish
	 * here.
	 */
	if (!slotname && rstates == NIL)
	{
		table_close(rel, NoLock);
		return;
	}

	/*
	 * Try to acquire the connection necessary for dropping slots.
	 *
	 * Note: If the slotname is NONE/NULL then we allow the command to finish
	 * and users need to manually cleanup the apply and tablesync worker slots
	 * later.
	 *
	 * This has to be at the end because otherwise if there is an error while
	 * doing the database operations we won't be able to rollback dropped
	 * slot.
	 */
	load_file("libpqwalreceiver", false);

	wrconn = walrcv_connect(conninfo, true, subname, &err);
	if (wrconn == NULL)
	{
		if (!slotname)
		{
			/* be tidy */
			list_free(rstates);
			table_close(rel, NoLock);
			return;
		}
		else
		{
			ReportSlotConnectionError(rstates, subid, slotname, err);
		}
	}

	PG_TRY();
	{
		foreach(lc, rstates)
		{
			SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);
			Oid			relid = rstate->relid;

			/* Only cleanup resources of tablesync workers */
			if (!OidIsValid(relid))
				continue;

			/*
			 * Drop the tablesync slots associated with removed tables.
			 *
			 * For SYNCDONE/READY states, the tablesync slot is known to have
			 * already been dropped by the tablesync worker.
			 *
			 * For other states, there is no certainty, maybe the slot does
			 * not exist yet. Also, if we fail after removing some of the
			 * slots, next time, it will again try to drop already dropped
			 * slots and fail. For these reasons, we allow missing_ok = true
			 * for the drop.
			 */
			if (rstate->state != SUBREL_STATE_SYNCDONE)
			{
				char		syncslotname[NAMEDATALEN] = {0};

				ReplicationSlotNameForTablesync(subid, relid, syncslotname,
												sizeof(syncslotname));
				ReplicationSlotDropAtPubNode(wrconn, syncslotname, true);
			}
		}

		list_free(rstates);

		/*
		 * If there is a slot associated with the subscription, then drop the
		 * replication slot at the publisher.
		 */
		if (slotname)
			ReplicationSlotDropAtPubNode(wrconn, slotname, false);
	}
	PG_FINALLY();
	{
		walrcv_disconnect(wrconn);
	}
	PG_END_TRY();

	table_close(rel, NoLock);
}

/*
 * Drop the replication slot at the publisher node using the replication
 * connection.
 *
 * missing_ok - if true then only issue a LOG message if the slot doesn't
 * exist.
 */
void
ReplicationSlotDropAtPubNode(WalReceiverConn *wrconn, char *slotname, bool missing_ok)
{
	StringInfoData cmd;

	Assert(wrconn);

	load_file("libpqwalreceiver", false);

	initStringInfo(&cmd);
	appendStringInfo(&cmd, "DROP_REPLICATION_SLOT %s WAIT", quote_identifier(slotname));

	PG_TRY();
	{
		WalRcvExecResult *res;

		res = walrcv_exec(wrconn, cmd.data, 0, NULL);

		if (res->status == WALRCV_OK_COMMAND)
		{
			/* NOTICE. Success. */
			ereport(NOTICE,
					(errmsg("dropped replication slot \"%s\" on publisher",
							slotname)));
		}
		else if (res->status == WALRCV_ERROR &&
				 missing_ok &&
				 res->sqlstate == ERRCODE_UNDEFINED_OBJECT)
		{
			/* LOG. Error, but missing_ok = true. */
			ereport(LOG,
					(errmsg("could not drop replication slot \"%s\" on publisher: %s",
							slotname, res->err)));
		}
		else
		{
			/* ERROR. */
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not drop replication slot \"%s\" on publisher: %s",
							slotname, res->err)));
		}

		walrcv_clear_result(res);
	}
	PG_FINALLY();
	{
		pfree(cmd.data);
	}
	PG_END_TRY();
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

	ApplyLauncherWakeupAtCommit();
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
 *
 * Note that we don't support the case where the column list is different for
 * the same table in different publications to avoid sending unwanted column
 * information for some of the rows. This can happen when both the column
 * list and row filter are specified for different publications.
 */
static List *
fetch_table_list(WalReceiverConn *wrconn, List *publications)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[3] = {TEXTOID, TEXTOID, NAMEARRAYOID};
	List	   *tablelist = NIL;
	bool		check_columnlist = (walrcv_server_version(wrconn) >= 150000);

	initStringInfo(&cmd);
	appendStringInfoString(&cmd, "SELECT DISTINCT t.schemaname, t.tablename \n");

	/* Get column lists for each relation if the publisher supports it */
	if (check_columnlist)
		appendStringInfoString(&cmd, ", t.attnames\n");

	appendStringInfoString(&cmd, "FROM pg_catalog.pg_publication_tables t\n"
						   " WHERE t.pubname IN (");
	get_publications_str(publications, &cmd, true);
	appendStringInfoChar(&cmd, ')');

	res = walrcv_exec(wrconn, cmd.data, check_columnlist ? 3 : 2, tableRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not receive list of replicated tables from the publisher: %s",
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

		rv = makeRangeVar(nspname, relname, -1);

		if (check_columnlist && list_member(tablelist, rv))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   nspname, relname));
		else
			tablelist = lappend(tablelist, rv);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	walrcv_clear_result(res);

	return tablelist;
}

/*
 * This is to report the connection failure while dropping replication slots.
 * Here, we report the WARNING for all tablesync slots so that user can drop
 * them manually, if required.
 */
static void
ReportSlotConnectionError(List *rstates, Oid subid, char *slotname, char *err)
{
	ListCell   *lc;

	foreach(lc, rstates)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);
		Oid			relid = rstate->relid;

		/* Only cleanup resources of tablesync workers */
		if (!OidIsValid(relid))
			continue;

		/*
		 * Caller needs to ensure that relstate doesn't change underneath us.
		 * See DropSubscription where we get the relstates.
		 */
		if (rstate->state != SUBREL_STATE_SYNCDONE)
		{
			char		syncslotname[NAMEDATALEN] = {0};

			ReplicationSlotNameForTablesync(subid, relid, syncslotname,
											sizeof(syncslotname));
			elog(WARNING, "could not drop tablesync replication slot \"%s\"",
				 syncslotname);
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_CONNECTION_FAILURE),
			 errmsg("could not connect to publisher when attempting to drop replication slot \"%s\": %s",
					slotname, err),
	/* translator: %s is an SQL ALTER command */
			 errhint("Use %s to disable the subscription, and then use %s to disassociate it from the slot.",
					 "ALTER SUBSCRIPTION ... DISABLE",
					 "ALTER SUBSCRIPTION ... SET (slot_name = NONE)")));
}

/*
 * Check for duplicates in the given list of publications and error out if
 * found one.  Add publications to datums as text datums, if datums is not
 * NULL.
 */
static void
check_duplicates_in_publist(List *publist, Datum *datums)
{
	ListCell   *cell;
	int			j = 0;

	foreach(cell, publist)
	{
		char	   *name = strVal(lfirst(cell));
		ListCell   *pcell;

		foreach(pcell, publist)
		{
			char	   *pname = strVal(lfirst(pcell));

			if (pcell == cell)
				break;

			if (strcmp(name, pname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("publication name \"%s\" used more than once",
								pname)));
		}

		if (datums)
			datums[j++] = CStringGetTextDatum(name);
	}
}

/*
 * Merge current subscription's publications and user-specified publications
 * from ADD/DROP PUBLICATIONS.
 *
 * If addpub is true, we will add the list of publications into oldpublist.
 * Otherwise, we will delete the list of publications from oldpublist.  The
 * returned list is a copy, oldpublist itself is not changed.
 *
 * subname is the subscription name, for error messages.
 */
static List *
merge_publications(List *oldpublist, List *newpublist, bool addpub, const char *subname)
{
	ListCell   *lc;

	oldpublist = list_copy(oldpublist);

	check_duplicates_in_publist(newpublist, NULL);

	foreach(lc, newpublist)
	{
		char	   *name = strVal(lfirst(lc));
		ListCell   *lc2;
		bool		found = false;

		foreach(lc2, oldpublist)
		{
			char	   *pubname = strVal(lfirst(lc2));

			if (strcmp(name, pubname) == 0)
			{
				found = true;
				if (addpub)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("publication \"%s\" is already in subscription \"%s\"",
									name, subname)));
				else
					oldpublist = foreach_delete_current(oldpublist, lc2);

				break;
			}
		}

		if (addpub && !found)
			oldpublist = lappend(oldpublist, makeString(name));
		else if (!addpub && !found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("publication \"%s\" is not in subscription \"%s\"",
							name, subname)));
	}

	/*
	 * XXX Probably no strong reason for this, but for now it's to make ALTER
	 * SUBSCRIPTION ... DROP PUBLICATION consistent with SET PUBLICATION.
	 */
	if (!oldpublist)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot drop all the publications from a subscription")));

	return oldpublist;
}
