/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL EVENT TRIGGER support code.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_database.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct EventTriggerQueryState
{
	/* memory context for this state's objects */
	MemoryContext cxt;

	/* sql_drop */
	slist_head	SQLDropList;
	bool		in_sql_drop;

	/* table_rewrite */
	Oid			table_rewrite_oid;	/* InvalidOid, or set for table_rewrite
									 * event */
	int			table_rewrite_reason;	/* AT_REWRITE reason */

	/* Support for command collection */
	bool		commandCollectionInhibited;
	CollectedCommand *currentCommand;
	List	   *commandList;	/* list of CollectedCommand; see
								 * deparse_utility.h */
	struct EventTriggerQueryState *previous;
} EventTriggerQueryState;

static EventTriggerQueryState *currentEventTriggerState = NULL;

/* GUC parameter */
bool		event_triggers = true;

/* Support for dropped objects */
typedef struct SQLDropObject
{
	ObjectAddress address;
	const char *schemaname;
	const char *objname;
	const char *objidentity;
	const char *objecttype;
	List	   *addrnames;
	List	   *addrargs;
	bool		original;
	bool		normal;
	bool		istemp;
	slist_node	next;
} SQLDropObject;

static void AlterEventTriggerOwner_internal(Relation rel,
											HeapTuple tup,
											Oid newOwnerId);
static void error_duplicate_filter_variable(const char *defname);
static Datum filter_list_to_array(List *filterlist);
static Oid	insert_event_trigger_tuple(const char *trigname, const char *eventname,
									   Oid evtOwner, Oid funcoid, List *taglist);
static void validate_ddl_tags(const char *filtervar, List *taglist);
static void validate_table_rewrite_tags(const char *filtervar, List *taglist);
static void EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata);
static const char *stringify_grant_objtype(ObjectType objtype);
static const char *stringify_adefprivs_objtype(ObjectType objtype);
static void SetDatabaseHasLoginEventTriggers(void);

/*
 * Create an event trigger.
 */
Oid
CreateEventTrigger(CreateEventTrigStmt *stmt)
{
	HeapTuple	tuple;
	Oid			funcoid;
	Oid			funcrettype;
	Oid			evtowner = GetUserId();
	ListCell   *lc;
	List	   *tags = NULL;

	/*
	 * It would be nice to allow database owners or even regular users to do
	 * this, but there are obvious privilege escalation risks which would have
	 * to somehow be plugged first.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create event trigger \"%s\"",
						stmt->trigname),
				 errhint("Must be superuser to create an event trigger.")));

	/* Validate event name. */
	if (strcmp(stmt->eventname, "ddl_command_start") != 0 &&
		strcmp(stmt->eventname, "ddl_command_end") != 0 &&
		strcmp(stmt->eventname, "sql_drop") != 0 &&
		strcmp(stmt->eventname, "login") != 0 &&
		strcmp(stmt->eventname, "table_rewrite") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unrecognized event name \"%s\"",
						stmt->eventname)));

	/* Validate filter conditions. */
	foreach(lc, stmt->whenclause)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "tag") == 0)
		{
			if (tags != NULL)
				error_duplicate_filter_variable(def->defname);
			tags = (List *) def->arg;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized filter variable \"%s\"", def->defname)));
	}

	/* Validate tag list, if any. */
	if ((strcmp(stmt->eventname, "ddl_command_start") == 0 ||
		 strcmp(stmt->eventname, "ddl_command_end") == 0 ||
		 strcmp(stmt->eventname, "sql_drop") == 0)
		&& tags != NULL)
		validate_ddl_tags("tag", tags);
	else if (strcmp(stmt->eventname, "table_rewrite") == 0
			 && tags != NULL)
		validate_table_rewrite_tags("tag", tags);
	else if (strcmp(stmt->eventname, "login") == 0 && tags != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("tag filtering is not supported for login event triggers")));

	/*
	 * Give user a nice error message if an event trigger of the same name
	 * already exists.
	 */
	tuple = SearchSysCache1(EVENTTRIGGERNAME, CStringGetDatum(stmt->trigname));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists",
						stmt->trigname)));

	/* Find and validate the trigger function. */
	funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != EVENT_TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->funcname), "event_trigger")));

	/* Insert catalog entries. */
	return insert_event_trigger_tuple(stmt->trigname, stmt->eventname,
									  evtowner, funcoid, tags);
}

/*
 * Validate DDL command tags.
 */
static void
validate_ddl_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tagstr = strVal(lfirst(lc));
		CommandTag	commandTag = GetCommandTagEnum(tagstr);

		if (commandTag == CMDTAG_UNKNOWN)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("filter value \"%s\" not recognized for filter variable \"%s\"",
							tagstr, filtervar)));
		if (!command_tag_event_trigger_ok(commandTag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
					 errmsg("event triggers are not supported for %s",
							tagstr)));
	}
}

/*
 * Validate DDL command tags for event table_rewrite.
 */
static void
validate_table_rewrite_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tagstr = strVal(lfirst(lc));
		CommandTag	commandTag = GetCommandTagEnum(tagstr);

		if (!command_tag_table_rewrite_ok(commandTag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
					 errmsg("event triggers are not supported for %s",
							tagstr)));
	}
}

/*
 * Complain about a duplicate filter variable.
 */
static void
error_duplicate_filter_variable(const char *defname)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("filter variable \"%s\" specified more than once",
					defname)));
}

/*
 * Insert the new pg_event_trigger row and record dependencies.
 */
static Oid
insert_event_trigger_tuple(const char *trigname, const char *eventname, Oid evtOwner,
						   Oid funcoid, List *taglist)
{
	Relation	tgrel;
	Oid			trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	NameData	evtnamedata,
				evteventdata;
	ObjectAddress myself,
				referenced;

	/* Open pg_event_trigger. */
	tgrel = table_open(EventTriggerRelationId, RowExclusiveLock);

	/* Build the new pg_trigger tuple. */
	trigoid = GetNewOidWithIndex(tgrel, EventTriggerOidIndexId,
								 Anum_pg_event_trigger_oid);
	values[Anum_pg_event_trigger_oid - 1] = ObjectIdGetDatum(trigoid);
	memset(nulls, false, sizeof(nulls));
	namestrcpy(&evtnamedata, trigname);
	values[Anum_pg_event_trigger_evtname - 1] = NameGetDatum(&evtnamedata);
	namestrcpy(&evteventdata, eventname);
	values[Anum_pg_event_trigger_evtevent - 1] = NameGetDatum(&evteventdata);
	values[Anum_pg_event_trigger_evtowner - 1] = ObjectIdGetDatum(evtOwner);
	values[Anum_pg_event_trigger_evtfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_event_trigger_evtenabled - 1] =
		CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);
	if (taglist == NIL)
		nulls[Anum_pg_event_trigger_evttags - 1] = true;
	else
		values[Anum_pg_event_trigger_evttags - 1] =
			filter_list_to_array(taglist);

	/* Insert heap tuple. */
	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);
	CatalogTupleInsert(tgrel, tuple);
	heap_freetuple(tuple);

	/*
	 * Login event triggers have an additional flag in pg_database to enable
	 * faster lookups in hot codepaths. Set the flag unless already True.
	 */
	if (strcmp(eventname, "login") == 0)
		SetDatabaseHasLoginEventTriggers();

	/* Depend on owner. */
	recordDependencyOnOwner(EventTriggerRelationId, trigoid, evtOwner);

	/* Depend on event trigger function. */
	myself.classId = EventTriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;
	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Depend on extension, if any. */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new event trigger */
	InvokeObjectPostCreateHook(EventTriggerRelationId, trigoid, 0);

	/* Close pg_event_trigger. */
	table_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
 * In the parser, a clause like WHEN tag IN ('cmd1', 'cmd2') is represented
 * by a DefElem whose value is a List of String nodes; in the catalog, we
 * store the list of strings as a text array.  This function transforms the
 * former representation into the latter one.
 *
 * For cleanliness, we store command tags in the catalog as text.  It's
 * possible (although not currently anticipated) that we might have
 * a case-sensitive filter variable in the future, in which case this would
 * need some further adjustment.
 */
static Datum
filter_list_to_array(List *filterlist)
{
	ListCell   *lc;
	Datum	   *data;
	int			i = 0,
				l = list_length(filterlist);

	data = (Datum *) palloc(l * sizeof(Datum));

	foreach(lc, filterlist)
	{
		const char *value = strVal(lfirst(lc));
		char	   *result,
				   *p;

		result = pstrdup(value);
		for (p = result; *p; p++)
			*p = pg_ascii_toupper((unsigned char) *p);
		data[i++] = PointerGetDatum(cstring_to_text(result));
		pfree(result);
	}

	return PointerGetDatum(construct_array_builtin(data, l, TEXTOID));
}

/*
 * Set pg_database.dathasloginevt flag for current database indicating that
 * current database has on login event triggers.
 */
void
SetDatabaseHasLoginEventTriggers(void)
{
	/* Set dathasloginevt flag in pg_database */
	Form_pg_database db;
	Relation	pg_db = table_open(DatabaseRelationId, RowExclusiveLock);
	ItemPointerData otid;
	HeapTuple	tuple;

	/*
	 * Use shared lock to prevent a conflict with EventTriggerOnLogin() trying
	 * to reset pg_database.dathasloginevt flag.  Note, this lock doesn't
	 * effectively blocks database or other objection.  It's just custom lock
	 * tag used to prevent multiple backends changing
	 * pg_database.dathasloginevt flag.
	 */
	LockSharedObject(DatabaseRelationId, MyDatabaseId, 0, AccessExclusiveLock);

	tuple = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	otid = tuple->t_self;
	db = (Form_pg_database) GETSTRUCT(tuple);
	if (!db->dathasloginevt)
	{
		db->dathasloginevt = true;
		CatalogTupleUpdate(pg_db, &otid, tuple);
		CommandCounterIncrement();
	}
	UnlockTuple(pg_db, &otid, InplaceUpdateTupleLock);
	table_close(pg_db, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER EVENT TRIGGER foo ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
Oid
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
	Oid			trigoid;
	Form_pg_event_trigger evtForm;
	char		tgenabled = stmt->tgenabled;

	tgrel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME,
							  CStringGetDatum(stmt->trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist",
						stmt->trigname)));

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	trigoid = evtForm->oid;

	if (!object_ownercheck(EventTriggerRelationId, trigoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EVENT_TRIGGER,
					   stmt->trigname);

	/* tuple is a copy, so we can modify it below */
	evtForm->evtenabled = tgenabled;

	CatalogTupleUpdate(tgrel, &tup->t_self, tup);

	/*
	 * Login event triggers have an additional flag in pg_database to enable
	 * faster lookups in hot codepaths. Set the flag unless already True.
	 */
	if (namestrcmp(&evtForm->evtevent, "login") == 0 &&
		tgenabled != TRIGGER_DISABLED)
		SetDatabaseHasLoginEventTriggers();

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  trigoid, 0);

	/* clean up */
	heap_freetuple(tup);
	table_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
 * Change event trigger's owner -- by name
 */
ObjectAddress
AlterEventTriggerOwner(const char *name, Oid newOwnerId)
{
	Oid			evtOid;
	HeapTuple	tup;
	Form_pg_event_trigger evtForm;
	Relation	rel;
	ObjectAddress address;

	rel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	evtOid = evtForm->oid;

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, EventTriggerRelationId, evtOid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change event trigger owner, by OID
 */
void
AlterEventTriggerOwner_oid(Oid trigOid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger with OID %u does not exist", trigOid)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Internal workhorse for changing an event trigger's owner
 */
static void
AlterEventTriggerOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_event_trigger form;

	form = (Form_pg_event_trigger) GETSTRUCT(tup);

	if (form->evtowner == newOwnerId)
		return;

	if (!object_ownercheck(EventTriggerRelationId, form->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EVENT_TRIGGER,
					   NameStr(form->evtname));

	/* New owner must be a superuser */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of event trigger \"%s\"",
						NameStr(form->evtname)),
				 errhint("The owner of an event trigger must be a superuser.")));

	form->evtowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Update owner dependency reference */
	changeDependencyOnOwner(EventTriggerRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  form->oid, 0);
}

/*
 * get_event_trigger_oid - Look up an event trigger by name to find its OID.
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 */
Oid
get_event_trigger_oid(const char *trigname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(EVENTTRIGGERNAME, Anum_pg_event_trigger_oid,
						  CStringGetDatum(trigname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", trigname)));
	return oid;
}

/*
 * Return true when we want to fire given Event Trigger and false otherwise,
 * filtering on the session replication role and the event trigger registered
 * tags matching.
 */
static bool
filter_event_trigger(CommandTag tag, EventTriggerCacheItem *item)
{
	/*
	 * Filter by session replication role, knowing that we never see disabled
	 * items down here.
	 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (item->enabled == TRIGGER_FIRES_ON_ORIGIN)
			return false;
	}
	else
	{
		if (item->enabled == TRIGGER_FIRES_ON_REPLICA)
			return false;
	}

	/* Filter by tags, if any were specified. */
	if (!bms_is_empty(item->tagset) && !bms_is_member(tag, item->tagset))
		return false;

	/* if we reach that point, we're not filtering out this item */
	return true;
}

static CommandTag
EventTriggerGetTag(Node *parsetree, EventTriggerEvent event)
{
	if (event == EVT_Login)
		return CMDTAG_LOGIN;
	else
		return CreateCommandTag(parsetree);
}

/*
 * Setup for running triggers for the given event.  Return value is an OID list
 * of functions to run; if there are any, trigdata is filled with an
 * appropriate EventTriggerData for them to receive.
 */
static List *
EventTriggerCommonSetup(Node *parsetree,
						EventTriggerEvent event, const char *eventstr,
						EventTriggerData *trigdata, bool unfiltered)
{
	CommandTag	tag;
	List	   *cachelist;
	ListCell   *lc;
	List	   *runlist = NIL;

	/*
	 * We want the list of command tags for which this procedure is actually
	 * invoked to match up exactly with the list that CREATE EVENT TRIGGER
	 * accepts.  This debugging cross-check will throw an error if this
	 * function is invoked for a command tag that CREATE EVENT TRIGGER won't
	 * accept.  (Unfortunately, there doesn't seem to be any simple, automated
	 * way to verify that CREATE EVENT TRIGGER doesn't accept extra stuff that
	 * never reaches this control point.)
	 *
	 * If this cross-check fails for you, you probably need to either adjust
	 * standard_ProcessUtility() not to invoke event triggers for the command
	 * type in question, or you need to adjust event_trigger_ok to accept the
	 * relevant command tag.
	 */
#ifdef USE_ASSERT_CHECKING
	{
		CommandTag	dbgtag;

		dbgtag = EventTriggerGetTag(parsetree, event);

		if (event == EVT_DDLCommandStart ||
			event == EVT_DDLCommandEnd ||
			event == EVT_SQLDrop ||
			event == EVT_Login)
		{
			if (!command_tag_event_trigger_ok(dbgtag))
				elog(ERROR, "unexpected command tag \"%s\"", GetCommandTagName(dbgtag));
		}
		else if (event == EVT_TableRewrite)
		{
			if (!command_tag_table_rewrite_ok(dbgtag))
				elog(ERROR, "unexpected command tag \"%s\"", GetCommandTagName(dbgtag));
		}
	}
#endif

	/* Use cache to find triggers for this event; fast exit if none. */
	cachelist = EventCacheLookup(event);
	if (cachelist == NIL)
		return NIL;

	/* Get the command tag. */
	tag = EventTriggerGetTag(parsetree, event);

	/*
	 * Filter list of event triggers by command tag, and copy them into our
	 * memory context.  Once we start running the command triggers, or indeed
	 * once we do anything at all that touches the catalogs, an invalidation
	 * might leave cachelist pointing at garbage, so we must do this before we
	 * can do much else.
	 */
	foreach(lc, cachelist)
	{
		EventTriggerCacheItem *item = lfirst(lc);

		if (unfiltered || filter_event_trigger(tag, item))
		{
			/* We must plan to fire this trigger. */
			runlist = lappend_oid(runlist, item->fnoid);
		}
	}

	/* Don't spend any more time on this if no functions to run */
	if (runlist == NIL)
		return NIL;

	trigdata->type = T_EventTriggerData;
	trigdata->event = eventstr;
	trigdata->parsetree = parsetree;
	trigdata->tag = tag;

	return runlist;
}

/*
 * Fire ddl_command_start triggers.
 */
void
EventTriggerDDLCommandStart(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * Event Triggers are completely disabled in standalone mode.  There are
	 * (at least) two reasons for this:
	 *
	 * 1. A sufficiently broken event trigger might not only render the
	 * database unusable, but prevent disabling itself to fix the situation.
	 * In this scenario, restarting in standalone mode provides an escape
	 * hatch.
	 *
	 * 2. BuildEventTriggerCache relies on systable_beginscan_ordered, and
	 * therefore will malfunction if pg_event_trigger's indexes are damaged.
	 * To allow recovery from a damaged index, we need some operating mode
	 * wherein event triggers are disabled.  (Or we could implement
	 * heapscan-and-sort logic for that case, but having disaster recovery
	 * scenarios depend on code that's otherwise untested isn't appetizing.)
	 *
	 * Additionally, event triggers can be disabled with a superuser-only GUC
	 * to make fixing database easier as per 1 above.
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandStart,
									  "ddl_command_start",
									  &trigdata, false);
	if (runlist == NIL)
		return;

	/* Run the triggers. */
	EventTriggerInvoke(runlist, &trigdata);

	/* Cleanup. */
	list_free(runlist);

	/*
	 * Make sure anything the event triggers did will be visible to the main
	 * command.
	 */
	CommandCounterIncrement();
}

/*
 * Fire ddl_command_end triggers.
 */
void
EventTriggerDDLCommandEnd(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via GUC.
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	/*
	 * Also do nothing if our state isn't set up, which it won't be if there
	 * weren't any relevant event triggers at the start of the current DDL
	 * command.  This test might therefore seem optional, but it's important
	 * because EventTriggerCommonSetup might find triggers that didn't exist
	 * at the time the command started.  Although this function itself
	 * wouldn't crash, the event trigger functions would presumably call
	 * pg_event_trigger_ddl_commands which would fail.  Better to do nothing
	 * until the next command.
	 */
	if (!currentEventTriggerState)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandEnd, "ddl_command_end",
									  &trigdata, false);
	if (runlist == NIL)
		return;

	/*
	 * Make sure anything the main command did will be visible to the event
	 * triggers.
	 */
	CommandCounterIncrement();

	/* Run the triggers. */
	EventTriggerInvoke(runlist, &trigdata);

	/* Cleanup. */
	list_free(runlist);
}

/*
 * Fire sql_drop triggers.
 */
void
EventTriggerSQLDrop(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via a GUC.
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	/*
	 * Use current state to determine whether this event fires at all.  If
	 * there are no triggers for the sql_drop event, then we don't have
	 * anything to do here.  Note that dropped object collection is disabled
	 * if this is the case, so even if we were to try to run, the list would
	 * be empty.
	 */
	if (!currentEventTriggerState ||
		slist_is_empty(&currentEventTriggerState->SQLDropList))
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_SQLDrop, "sql_drop",
									  &trigdata, false);

	/*
	 * Nothing to do if run list is empty.  Note this typically can't happen,
	 * because if there are no sql_drop events, then objects-to-drop wouldn't
	 * have been collected in the first place and we would have quit above.
	 * But it could occur if event triggers were dropped partway through.
	 */
	if (runlist == NIL)
		return;

	/*
	 * Make sure anything the main command did will be visible to the event
	 * triggers.
	 */
	CommandCounterIncrement();

	/*
	 * Make sure pg_event_trigger_dropped_objects only works when running
	 * these triggers.  Use PG_TRY to ensure in_sql_drop is reset even when
	 * one trigger fails.  (This is perhaps not necessary, as the currentState
	 * variable will be removed shortly by our caller, but it seems better to
	 * play safe.)
	 */
	currentEventTriggerState->in_sql_drop = true;

	/* Run the triggers. */
	PG_TRY();
	{
		EventTriggerInvoke(runlist, &trigdata);
	}
	PG_FINALLY();
	{
		currentEventTriggerState->in_sql_drop = false;
	}
	PG_END_TRY();

	/* Cleanup. */
	list_free(runlist);
}

/*
 * Fire login event triggers if any are present.  The dathasloginevt
 * pg_database flag is left unchanged when an event trigger is dropped to avoid
 * complicating the codepath in the case of multiple event triggers.  This
 * function will instead unset the flag if no trigger is defined.
 */
void
EventTriggerOnLogin(void)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via a GUC.  We also need a
	 * database connection (some background workers don't have it).
	 */
	if (!IsUnderPostmaster || !event_triggers ||
		!OidIsValid(MyDatabaseId) || !MyDatabaseHasLoginEventTriggers)
		return;

	StartTransactionCommand();
	runlist = EventTriggerCommonSetup(NULL,
									  EVT_Login, "login",
									  &trigdata, false);

	if (runlist != NIL)
	{
		/*
		 * Event trigger execution may require an active snapshot.
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* Run the triggers. */
		EventTriggerInvoke(runlist, &trigdata);

		/* Cleanup. */
		list_free(runlist);

		PopActiveSnapshot();
	}

	/*
	 * There is no active login event trigger, but our
	 * pg_database.dathasloginevt is set. Try to unset this flag.  We use the
	 * lock to prevent concurrent SetDatabaseHasLoginEventTriggers(), but we
	 * don't want to hang the connection waiting on the lock.  Thus, we are
	 * just trying to acquire the lock conditionally.
	 */
	else if (ConditionalLockSharedObject(DatabaseRelationId, MyDatabaseId,
										 0, AccessExclusiveLock))
	{
		/*
		 * The lock is held.  Now we need to recheck that login event triggers
		 * list is still empty.  Once the list is empty, we know that even if
		 * there is a backend which concurrently inserts/enables a login event
		 * trigger, it will update pg_database.dathasloginevt *afterwards*.
		 */
		runlist = EventTriggerCommonSetup(NULL,
										  EVT_Login, "login",
										  &trigdata, true);

		if (runlist == NIL)
		{
			Relation	pg_db = table_open(DatabaseRelationId, RowExclusiveLock);
			HeapTuple	tuple;
			void	   *state;
			Form_pg_database db;
			ScanKeyData key[1];

			/* Fetch a copy of the tuple to scribble on */
			ScanKeyInit(&key[0],
						Anum_pg_database_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(MyDatabaseId));

			systable_inplace_update_begin(pg_db, DatabaseOidIndexId, true,
										  NULL, 1, key, &tuple, &state);

			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "could not find tuple for database %u", MyDatabaseId);

			db = (Form_pg_database) GETSTRUCT(tuple);
			if (db->dathasloginevt)
			{
				db->dathasloginevt = false;

				/*
				 * Do an "in place" update of the pg_database tuple.  Doing
				 * this instead of regular updates serves two purposes. First,
				 * that avoids possible waiting on the row-level lock. Second,
				 * that avoids dealing with TOAST.
				 */
				systable_inplace_update_finish(state, tuple);
			}
			else
				systable_inplace_update_cancel(state);
			table_close(pg_db, RowExclusiveLock);
			heap_freetuple(tuple);
		}
		else
		{
			list_free(runlist);
		}
	}
	CommitTransactionCommand();
}


/*
 * Fire table_rewrite triggers.
 */
void
EventTriggerTableRewrite(Node *parsetree, Oid tableOid, int reason)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via a GUC.
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	/*
	 * Also do nothing if our state isn't set up, which it won't be if there
	 * weren't any relevant event triggers at the start of the current DDL
	 * command.  This test might therefore seem optional, but it's
	 * *necessary*, because EventTriggerCommonSetup might find triggers that
	 * didn't exist at the time the command started.
	 */
	if (!currentEventTriggerState)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_TableRewrite,
									  "table_rewrite",
									  &trigdata, false);
	if (runlist == NIL)
		return;

	/*
	 * Make sure pg_event_trigger_table_rewrite_oid only works when running
	 * these triggers. Use PG_TRY to ensure table_rewrite_oid is reset even
	 * when one trigger fails. (This is perhaps not necessary, as the
	 * currentState variable will be removed shortly by our caller, but it
	 * seems better to play safe.)
	 */
	currentEventTriggerState->table_rewrite_oid = tableOid;
	currentEventTriggerState->table_rewrite_reason = reason;

	/* Run the triggers. */
	PG_TRY();
	{
		EventTriggerInvoke(runlist, &trigdata);
	}
	PG_FINALLY();
	{
		currentEventTriggerState->table_rewrite_oid = InvalidOid;
		currentEventTriggerState->table_rewrite_reason = 0;
	}
	PG_END_TRY();

	/* Cleanup. */
	list_free(runlist);

	/*
	 * Make sure anything the event triggers did will be visible to the main
	 * command.
	 */
	CommandCounterIncrement();
}

/*
 * Invoke each event trigger in a list of event triggers.
 */
static void
EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata)
{
	MemoryContext context;
	MemoryContext oldcontext;
	ListCell   *lc;
	bool		first = true;

	/* Guard against stack overflow due to recursive event trigger */
	check_stack_depth();

	/*
	 * Let's evaluate event triggers in their own memory context, so that any
	 * leaks get cleaned up promptly.
	 */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"event trigger context",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);

	/* Call each event trigger. */
	foreach(lc, fn_oid_list)
	{
		LOCAL_FCINFO(fcinfo, 0);
		Oid			fnoid = lfirst_oid(lc);
		FmgrInfo	flinfo;
		PgStat_FunctionCallUsage fcusage;

		elog(DEBUG1, "EventTriggerInvoke %u", fnoid);

		/*
		 * We want each event trigger to be able to see the results of the
		 * previous event trigger's action.  Caller is responsible for any
		 * command-counter increment that is needed between the event trigger
		 * and anything else in the transaction.
		 */
		if (first)
			first = false;
		else
			CommandCounterIncrement();

		/* Look up the function */
		fmgr_info(fnoid, &flinfo);

		/* Call the function, passing no arguments but setting a context. */
		InitFunctionCallInfoData(*fcinfo, &flinfo, 0,
								 InvalidOid, (Node *) trigdata, NULL);
		pgstat_init_function_usage(fcinfo, &fcusage);
		FunctionCallInvoke(fcinfo);
		pgstat_end_function_usage(&fcusage, true);

		/* Reclaim memory. */
		MemoryContextReset(context);
	}

	/* Restore old memory context and delete the temporary one. */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
}

/*
 * Do event triggers support this object type?
 *
 * See also event trigger support matrix in event-trigger.sgml.
 */
bool
EventTriggerSupportsObjectType(ObjectType obtype)
{
	switch (obtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_TABLESPACE:
		case OBJECT_ROLE:
		case OBJECT_PARAMETER_ACL:
			/* no support for global objects (except subscriptions) */
			return false;
		case OBJECT_EVENT_TRIGGER:
			/* no support for event triggers on event triggers */
			return false;
		default:
			return true;
	}
}

/*
 * Do event triggers support this object class?
 *
 * See also event trigger support matrix in event-trigger.sgml.
 */
bool
EventTriggerSupportsObject(const ObjectAddress *object)
{
	switch (object->classId)
	{
		case DatabaseRelationId:
		case TableSpaceRelationId:
		case AuthIdRelationId:
		case AuthMemRelationId:
		case ParameterAclRelationId:
			/* no support for global objects (except subscriptions) */
			return false;
		case EventTriggerRelationId:
			/* no support for event triggers on event triggers */
			return false;
		default:
			return true;
	}
}

/*
 * Prepare event trigger state for a new complete query to run, if necessary;
 * returns whether this was done.  If it was, EventTriggerEndCompleteQuery must
 * be called when the query is done, regardless of whether it succeeds or fails
 * -- so use of a PG_TRY block is mandatory.
 */
bool
EventTriggerBeginCompleteQuery(void)
{
	EventTriggerQueryState *state;
	MemoryContext cxt;

	/*
	 * Currently, sql_drop, table_rewrite, ddl_command_end events are the only
	 * reason to have event trigger state at all; so if there are none, don't
	 * install one.
	 */
	if (!trackDroppedObjectsNeeded())
		return false;

	cxt = AllocSetContextCreate(TopMemoryContext,
								"event trigger state",
								ALLOCSET_DEFAULT_SIZES);
	state = MemoryContextAlloc(cxt, sizeof(EventTriggerQueryState));
	state->cxt = cxt;
	slist_init(&(state->SQLDropList));
	state->in_sql_drop = false;
	state->table_rewrite_oid = InvalidOid;

	state->commandCollectionInhibited = currentEventTriggerState ?
		currentEventTriggerState->commandCollectionInhibited : false;
	state->currentCommand = NULL;
	state->commandList = NIL;
	state->previous = currentEventTriggerState;
	currentEventTriggerState = state;

	return true;
}

/*
 * Query completed (or errored out) -- clean up local state, return to previous
 * one.
 *
 * Note: it's an error to call this routine if EventTriggerBeginCompleteQuery
 * returned false previously.
 *
 * Note: this might be called in the PG_CATCH block of a failing transaction,
 * so be wary of running anything unnecessary.  (In particular, it's probably
 * unwise to try to allocate memory.)
 */
void
EventTriggerEndCompleteQuery(void)
{
	EventTriggerQueryState *prevstate;

	prevstate = currentEventTriggerState->previous;

	/* this avoids the need for retail pfree of SQLDropList items: */
	MemoryContextDelete(currentEventTriggerState->cxt);

	currentEventTriggerState = prevstate;
}

/*
 * Do we need to keep close track of objects being dropped?
 *
 * This is useful because there is a cost to running with them enabled.
 */
bool
trackDroppedObjectsNeeded(void)
{
	/*
	 * true if any sql_drop, table_rewrite, ddl_command_end event trigger
	 * exists
	 */
	return (EventCacheLookup(EVT_SQLDrop) != NIL) ||
		(EventCacheLookup(EVT_TableRewrite) != NIL) ||
		(EventCacheLookup(EVT_DDLCommandEnd) != NIL);
}

/*
 * Support for dropped objects information on event trigger functions.
 *
 * We keep the list of objects dropped by the current command in current
 * state's SQLDropList (comprising SQLDropObject items).  Each time a new
 * command is to start, a clean EventTriggerQueryState is created; commands
 * that drop objects do the dependency.c dance to drop objects, which
 * populates the current state's SQLDropList; when the event triggers are
 * invoked they can consume the list via pg_event_trigger_dropped_objects().
 * When the command finishes, the EventTriggerQueryState is cleared, and
 * the one from the previous command is restored (when no command is in
 * execution, the current state is NULL).
 *
 * All this lets us support the case that an event trigger function drops
 * objects "reentrantly".
 */

/*
 * Register one object as being dropped by the current command.
 */
void
EventTriggerSQLDropAddObject(const ObjectAddress *object, bool original, bool normal)
{
	SQLDropObject *obj;
	MemoryContext oldcxt;

	if (!currentEventTriggerState)
		return;

	Assert(EventTriggerSupportsObject(object));

	/* don't report temp schemas except my own */
	if (object->classId == NamespaceRelationId &&
		(isAnyTempNamespace(object->objectId) &&
		 !isTempNamespace(object->objectId)))
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	obj = palloc0(sizeof(SQLDropObject));
	obj->address = *object;
	obj->original = original;
	obj->normal = normal;

	/*
	 * Obtain schema names from the object's catalog tuple, if one exists;
	 * this lets us skip objects in temp schemas.  We trust that
	 * ObjectProperty contains all object classes that can be
	 * schema-qualified.
	 */
	if (is_objectclass_supported(object->classId))
	{
		Relation	catalog;
		HeapTuple	tuple;

		catalog = table_open(obj->address.classId, AccessShareLock);
		tuple = get_catalog_object_by_oid(catalog,
										  get_object_attnum_oid(object->classId),
										  obj->address.objectId);

		if (tuple)
		{
			AttrNumber	attnum;
			Datum		datum;
			bool		isnull;

			attnum = get_object_attnum_namespace(obj->address.classId);
			if (attnum != InvalidAttrNumber)
			{
				datum = heap_getattr(tuple, attnum,
									 RelationGetDescr(catalog), &isnull);
				if (!isnull)
				{
					Oid			namespaceId;

					namespaceId = DatumGetObjectId(datum);
					/* temp objects are only reported if they are my own */
					if (isTempNamespace(namespaceId))
					{
						obj->schemaname = "pg_temp";
						obj->istemp = true;
					}
					else if (isAnyTempNamespace(namespaceId))
					{
						pfree(obj);
						table_close(catalog, AccessShareLock);
						MemoryContextSwitchTo(oldcxt);
						return;
					}
					else
					{
						obj->schemaname = get_namespace_name(namespaceId);
						obj->istemp = false;
					}
				}
			}

			if (get_object_namensp_unique(obj->address.classId) &&
				obj->address.objectSubId == 0)
			{
				attnum = get_object_attnum_name(obj->address.classId);
				if (attnum != InvalidAttrNumber)
				{
					datum = heap_getattr(tuple, attnum,
										 RelationGetDescr(catalog), &isnull);
					if (!isnull)
						obj->objname = pstrdup(NameStr(*DatumGetName(datum)));
				}
			}
		}

		table_close(catalog, AccessShareLock);
	}
	else
	{
		if (object->classId == NamespaceRelationId &&
			isTempNamespace(object->objectId))
			obj->istemp = true;
	}

	/* object identity, objname and objargs */
	obj->objidentity =
		getObjectIdentityParts(&obj->address, &obj->addrnames, &obj->addrargs,
							   false);

	/* object type */
	obj->objecttype = getObjectTypeDescription(&obj->address, false);

	slist_push_head(&(currentEventTriggerState->SQLDropList), &obj->next);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * pg_event_trigger_dropped_objects
 *
 * Make the list of dropped objects available to the user function run by the
 * Event Trigger.
 */
Datum
pg_event_trigger_dropped_objects(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	slist_iter	iter;

	/*
	 * Protect this function from being called out of context
	 */
	if (!currentEventTriggerState ||
		!currentEventTriggerState->in_sql_drop)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a sql_drop event trigger function",
						"pg_event_trigger_dropped_objects()")));

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	slist_foreach(iter, &(currentEventTriggerState->SQLDropList))
	{
		SQLDropObject *obj;
		int			i = 0;
		Datum		values[12] = {0};
		bool		nulls[12] = {0};

		obj = slist_container(SQLDropObject, next, iter.cur);

		/* classid */
		values[i++] = ObjectIdGetDatum(obj->address.classId);

		/* objid */
		values[i++] = ObjectIdGetDatum(obj->address.objectId);

		/* objsubid */
		values[i++] = Int32GetDatum(obj->address.objectSubId);

		/* original */
		values[i++] = BoolGetDatum(obj->original);

		/* normal */
		values[i++] = BoolGetDatum(obj->normal);

		/* is_temporary */
		values[i++] = BoolGetDatum(obj->istemp);

		/* object_type */
		values[i++] = CStringGetTextDatum(obj->objecttype);

		/* schema_name */
		if (obj->schemaname)
			values[i++] = CStringGetTextDatum(obj->schemaname);
		else
			nulls[i++] = true;

		/* object_name */
		if (obj->objname)
			values[i++] = CStringGetTextDatum(obj->objname);
		else
			nulls[i++] = true;

		/* object_identity */
		if (obj->objidentity)
			values[i++] = CStringGetTextDatum(obj->objidentity);
		else
			nulls[i++] = true;

		/* address_names and address_args */
		if (obj->addrnames)
		{
			values[i++] = PointerGetDatum(strlist_to_textarray(obj->addrnames));

			if (obj->addrargs)
				values[i++] = PointerGetDatum(strlist_to_textarray(obj->addrargs));
			else
				values[i++] = PointerGetDatum(construct_empty_array(TEXTOID));
		}
		else
		{
			nulls[i++] = true;
			nulls[i++] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * pg_event_trigger_table_rewrite_oid
 *
 * Make the Oid of the table going to be rewritten available to the user
 * function run by the Event Trigger.
 */
Datum
pg_event_trigger_table_rewrite_oid(PG_FUNCTION_ARGS)
{
	/*
	 * Protect this function from being called out of context
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->table_rewrite_oid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a table_rewrite event trigger function",
						"pg_event_trigger_table_rewrite_oid()")));

	PG_RETURN_OID(currentEventTriggerState->table_rewrite_oid);
}

/*
 * pg_event_trigger_table_rewrite_reason
 *
 * Make the rewrite reason available to the user.
 */
Datum
pg_event_trigger_table_rewrite_reason(PG_FUNCTION_ARGS)
{
	/*
	 * Protect this function from being called out of context
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->table_rewrite_reason == 0)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a table_rewrite event trigger function",
						"pg_event_trigger_table_rewrite_reason()")));

	PG_RETURN_INT32(currentEventTriggerState->table_rewrite_reason);
}

/*-------------------------------------------------------------------------
 * Support for DDL command deparsing
 *
 * The routines below enable an event trigger function to obtain a list of
 * DDL commands as they are executed.  There are three main pieces to this
 * feature:
 *
 * 1) Within ProcessUtilitySlow, or some sub-routine thereof, each DDL command
 * adds a struct CollectedCommand representation of itself to the command list,
 * using the routines below.
 *
 * 2) Some time after that, ddl_command_end fires and the command list is made
 * available to the event trigger function via pg_event_trigger_ddl_commands();
 * the complete command details are exposed as a column of type pg_ddl_command.
 *
 * 3) An extension can install a function capable of taking a value of type
 * pg_ddl_command and transform it into some external, user-visible and/or
 * -modifiable representation.
 *-------------------------------------------------------------------------
 */

/*
 * Inhibit DDL command collection.
 */
void
EventTriggerInhibitCommandCollection(void)
{
	if (!currentEventTriggerState)
		return;

	currentEventTriggerState->commandCollectionInhibited = true;
}

/*
 * Re-establish DDL command collection.
 */
void
EventTriggerUndoInhibitCommandCollection(void)
{
	if (!currentEventTriggerState)
		return;

	currentEventTriggerState->commandCollectionInhibited = false;
}

/*
 * EventTriggerCollectSimpleCommand
 *		Save data about a simple DDL command that was just executed
 *
 * address identifies the object being operated on.  secondaryObject is an
 * object address that was related in some way to the executed command; its
 * meaning is command-specific.
 *
 * For instance, for an ALTER obj SET SCHEMA command, objtype is the type of
 * object being moved, objectId is its OID, and secondaryOid is the OID of the
 * old schema.  (The destination schema OID can be obtained by catalog lookup
 * of the object.)
 */
void
EventTriggerCollectSimpleCommand(ObjectAddress address,
								 ObjectAddress secondaryObject,
								 Node *parsetree)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));

	command->type = SCT_Simple;
	command->in_extension = creating_extension;

	command->d.simple.address = address;
	command->d.simple.secondaryObject = secondaryObject;
	command->parsetree = copyObject(parsetree);

	currentEventTriggerState->commandList = lappend(currentEventTriggerState->commandList,
													command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerAlterTableStart
 *		Prepare to receive data on an ALTER TABLE command about to be executed
 *
 * Note we don't collect the command immediately; instead we keep it in
 * currentCommand, and only when we're done processing the subcommands we will
 * add it to the command list.
 */
void
EventTriggerAlterTableStart(Node *parsetree)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));

	command->type = SCT_AlterTable;
	command->in_extension = creating_extension;

	command->d.alterTable.classId = RelationRelationId;
	command->d.alterTable.objectId = InvalidOid;
	command->d.alterTable.subcmds = NIL;
	command->parsetree = copyObject(parsetree);

	command->parent = currentEventTriggerState->currentCommand;
	currentEventTriggerState->currentCommand = command;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Remember the OID of the object being affected by an ALTER TABLE.
 *
 * This is needed because in some cases we don't know the OID until later.
 */
void
EventTriggerAlterTableRelid(Oid objectId)
{
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	currentEventTriggerState->currentCommand->d.alterTable.objectId = objectId;
}

/*
 * EventTriggerCollectAlterTableSubcmd
 *		Save data about a single part of an ALTER TABLE.
 *
 * Several different commands go through this path, but apart from ALTER TABLE
 * itself, they are all concerned with AlterTableCmd nodes that are generated
 * internally, so that's all that this code needs to handle at the moment.
 */
void
EventTriggerCollectAlterTableSubcmd(Node *subcmd, ObjectAddress address)
{
	MemoryContext oldcxt;
	CollectedATSubcmd *newsub;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	Assert(IsA(subcmd, AlterTableCmd));
	Assert(currentEventTriggerState->currentCommand != NULL);
	Assert(OidIsValid(currentEventTriggerState->currentCommand->d.alterTable.objectId));

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	newsub = palloc(sizeof(CollectedATSubcmd));
	newsub->address = address;
	newsub->parsetree = copyObject(subcmd);

	currentEventTriggerState->currentCommand->d.alterTable.subcmds =
		lappend(currentEventTriggerState->currentCommand->d.alterTable.subcmds, newsub);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerAlterTableEnd
 *		Finish up saving an ALTER TABLE command, and add it to command list.
 *
 * FIXME this API isn't considering the possibility that an xact/subxact is
 * aborted partway through.  Probably it's best to add an
 * AtEOSubXact_EventTriggers() to fix this.
 */
void
EventTriggerAlterTableEnd(void)
{
	CollectedCommand *parent;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	parent = currentEventTriggerState->currentCommand->parent;

	/* If no subcommands, don't collect */
	if (currentEventTriggerState->currentCommand->d.alterTable.subcmds != NIL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

		currentEventTriggerState->commandList =
			lappend(currentEventTriggerState->commandList,
					currentEventTriggerState->currentCommand);

		MemoryContextSwitchTo(oldcxt);
	}
	else
		pfree(currentEventTriggerState->currentCommand);

	currentEventTriggerState->currentCommand = parent;
}

/*
 * EventTriggerCollectGrant
 *		Save data about a GRANT/REVOKE command being executed
 *
 * This function creates a copy of the InternalGrant, as the original might
 * not have the right lifetime.
 */
void
EventTriggerCollectGrant(InternalGrant *istmt)
{
	MemoryContext oldcxt;
	CollectedCommand *command;
	InternalGrant *icopy;
	ListCell   *cell;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	/*
	 * This is tedious, but necessary.
	 */
	icopy = palloc(sizeof(InternalGrant));
	memcpy(icopy, istmt, sizeof(InternalGrant));
	icopy->objects = list_copy(istmt->objects);
	icopy->grantees = list_copy(istmt->grantees);
	icopy->col_privs = NIL;
	foreach(cell, istmt->col_privs)
		icopy->col_privs = lappend(icopy->col_privs, copyObject(lfirst(cell)));

	/* Now collect it, using the copied InternalGrant */
	command = palloc(sizeof(CollectedCommand));
	command->type = SCT_Grant;
	command->in_extension = creating_extension;
	command->d.grant.istmt = icopy;
	command->parsetree = NULL;

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterOpFam
 *		Save data about an ALTER OPERATOR FAMILY ADD/DROP command being
 *		executed
 */
void
EventTriggerCollectAlterOpFam(AlterOpFamilyStmt *stmt, Oid opfamoid,
							  List *operators, List *procedures)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));
	command->type = SCT_AlterOpFamily;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.opfam.address,
					 OperatorFamilyRelationId, opfamoid);
	command->d.opfam.operators = operators;
	command->d.opfam.procedures = procedures;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectCreateOpClass
 *		Save data about a CREATE OPERATOR CLASS command being executed
 */
void
EventTriggerCollectCreateOpClass(CreateOpClassStmt *stmt, Oid opcoid,
								 List *operators, List *procedures)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_CreateOpClass;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.createopc.address,
					 OperatorClassRelationId, opcoid);
	command->d.createopc.operators = operators;
	command->d.createopc.procedures = procedures;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterTSConfig
 *		Save data about an ALTER TEXT SEARCH CONFIGURATION command being
 *		executed
 */
void
EventTriggerCollectAlterTSConfig(AlterTSConfigurationStmt *stmt, Oid cfgId,
								 Oid *dictIds, int ndicts)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_AlterTSConfig;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.atscfg.address,
					 TSConfigRelationId, cfgId);
	command->d.atscfg.dictIds = palloc(sizeof(Oid) * ndicts);
	memcpy(command->d.atscfg.dictIds, dictIds, sizeof(Oid) * ndicts);
	command->d.atscfg.ndicts = ndicts;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterDefPrivs
 *		Save data about an ALTER DEFAULT PRIVILEGES command being
 *		executed
 */
void
EventTriggerCollectAlterDefPrivs(AlterDefaultPrivilegesStmt *stmt)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_AlterDefaultPrivileges;
	command->d.defprivs.objtype = stmt->action->objtype;
	command->in_extension = creating_extension;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * In a ddl_command_end event trigger, this function reports the DDL commands
 * being run.
 */
Datum
pg_event_trigger_ddl_commands(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	/*
	 * Protect this function from being called out of context
	 */
	if (!currentEventTriggerState)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in an event trigger function",
						"pg_event_trigger_ddl_commands()")));

	/* Build tuplestore to hold the result rows */
	InitMaterializedSRF(fcinfo, 0);

	foreach(lc, currentEventTriggerState->commandList)
	{
		CollectedCommand *cmd = lfirst(lc);
		Datum		values[9];
		bool		nulls[9] = {0};
		ObjectAddress addr;
		int			i = 0;

		/*
		 * For IF NOT EXISTS commands that attempt to create an existing
		 * object, the returned OID is Invalid.  Don't return anything.
		 *
		 * One might think that a viable alternative would be to look up the
		 * Oid of the existing object and run the deparse with that.  But
		 * since the parse tree might be different from the one that created
		 * the object in the first place, we might not end up in a consistent
		 * state anyway.
		 */
		if (cmd->type == SCT_Simple &&
			!OidIsValid(cmd->d.simple.address.objectId))
			continue;

		switch (cmd->type)
		{
			case SCT_Simple:
			case SCT_AlterTable:
			case SCT_AlterOpFamily:
			case SCT_CreateOpClass:
			case SCT_AlterTSConfig:
				{
					char	   *identity;
					char	   *type;
					char	   *schema = NULL;

					if (cmd->type == SCT_Simple)
						addr = cmd->d.simple.address;
					else if (cmd->type == SCT_AlterTable)
						ObjectAddressSet(addr,
										 cmd->d.alterTable.classId,
										 cmd->d.alterTable.objectId);
					else if (cmd->type == SCT_AlterOpFamily)
						addr = cmd->d.opfam.address;
					else if (cmd->type == SCT_CreateOpClass)
						addr = cmd->d.createopc.address;
					else if (cmd->type == SCT_AlterTSConfig)
						addr = cmd->d.atscfg.address;

					/*
					 * If an object was dropped in the same command we may end
					 * up in a situation where we generated a message but can
					 * no longer look for the object information, so skip it
					 * rather than failing.  This can happen for example with
					 * some subcommand combinations of ALTER TABLE.
					 */
					identity = getObjectIdentity(&addr, true);
					if (identity == NULL)
						continue;

					/* The type can never be NULL. */
					type = getObjectTypeDescription(&addr, true);

					/*
					 * Obtain schema name, if any ("pg_temp" if a temp
					 * object). If the object class is not in the supported
					 * list here, we assume it's a schema-less object type,
					 * and thus "schema" remains set to NULL.
					 */
					if (is_objectclass_supported(addr.classId))
					{
						AttrNumber	nspAttnum;

						nspAttnum = get_object_attnum_namespace(addr.classId);
						if (nspAttnum != InvalidAttrNumber)
						{
							Relation	catalog;
							HeapTuple	objtup;
							Oid			schema_oid;
							bool		isnull;

							catalog = table_open(addr.classId, AccessShareLock);
							objtup = get_catalog_object_by_oid(catalog,
															   get_object_attnum_oid(addr.classId),
															   addr.objectId);
							if (!HeapTupleIsValid(objtup))
								elog(ERROR, "cache lookup failed for object %u/%u",
									 addr.classId, addr.objectId);
							schema_oid =
								heap_getattr(objtup, nspAttnum,
											 RelationGetDescr(catalog), &isnull);
							if (isnull)
								elog(ERROR,
									 "invalid null namespace in object %u/%u/%d",
									 addr.classId, addr.objectId, addr.objectSubId);
							schema = get_namespace_name_or_temp(schema_oid);

							table_close(catalog, AccessShareLock);
						}
					}

					/* classid */
					values[i++] = ObjectIdGetDatum(addr.classId);
					/* objid */
					values[i++] = ObjectIdGetDatum(addr.objectId);
					/* objsubid */
					values[i++] = Int32GetDatum(addr.objectSubId);
					/* command tag */
					values[i++] = CStringGetTextDatum(CreateCommandName(cmd->parsetree));
					/* object_type */
					values[i++] = CStringGetTextDatum(type);
					/* schema */
					if (schema == NULL)
						nulls[i++] = true;
					else
						values[i++] = CStringGetTextDatum(schema);
					/* identity */
					values[i++] = CStringGetTextDatum(identity);
					/* in_extension */
					values[i++] = BoolGetDatum(cmd->in_extension);
					/* command */
					values[i++] = PointerGetDatum(cmd);
				}
				break;

			case SCT_AlterDefaultPrivileges:
				/* classid */
				nulls[i++] = true;
				/* objid */
				nulls[i++] = true;
				/* objsubid */
				nulls[i++] = true;
				/* command tag */
				values[i++] = CStringGetTextDatum(CreateCommandName(cmd->parsetree));
				/* object_type */
				values[i++] = CStringGetTextDatum(stringify_adefprivs_objtype(cmd->d.defprivs.objtype));
				/* schema */
				nulls[i++] = true;
				/* identity */
				nulls[i++] = true;
				/* in_extension */
				values[i++] = BoolGetDatum(cmd->in_extension);
				/* command */
				values[i++] = PointerGetDatum(cmd);
				break;

			case SCT_Grant:
				/* classid */
				nulls[i++] = true;
				/* objid */
				nulls[i++] = true;
				/* objsubid */
				nulls[i++] = true;
				/* command tag */
				values[i++] = CStringGetTextDatum(cmd->d.grant.istmt->is_grant ?
												  "GRANT" : "REVOKE");
				/* object_type */
				values[i++] = CStringGetTextDatum(stringify_grant_objtype(cmd->d.grant.istmt->objtype));
				/* schema */
				nulls[i++] = true;
				/* identity */
				nulls[i++] = true;
				/* in_extension */
				values[i++] = BoolGetDatum(cmd->in_extension);
				/* command */
				values[i++] = PointerGetDatum(cmd);
				break;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	PG_RETURN_VOID();
}

/*
 * Return the ObjectType as a string, as it would appear in GRANT and
 * REVOKE commands.
 */
static const char *
stringify_grant_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return "COLUMN";
		case OBJECT_TABLE:
			return "TABLE";
		case OBJECT_SEQUENCE:
			return "SEQUENCE";
		case OBJECT_DATABASE:
			return "DATABASE";
		case OBJECT_DOMAIN:
			return "DOMAIN";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPER";
		case OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVER";
		case OBJECT_FUNCTION:
			return "FUNCTION";
		case OBJECT_LANGUAGE:
			return "LANGUAGE";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECT";
		case OBJECT_SCHEMA:
			return "SCHEMA";
		case OBJECT_PARAMETER_ACL:
			return "PARAMETER";
		case OBJECT_PROCEDURE:
			return "PROCEDURE";
		case OBJECT_ROUTINE:
			return "ROUTINE";
		case OBJECT_TABLESPACE:
			return "TABLESPACE";
		case OBJECT_TYPE:
			return "TYPE";
			/* these currently aren't used */
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROLE:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			elog(ERROR, "unsupported object type: %d", (int) objtype);
	}

	return "???";				/* keep compiler quiet */
}

/*
 * Return the ObjectType as a string; as above, but use the spelling
 * in ALTER DEFAULT PRIVILEGES commands instead.  Generally this is just
 * the plural.
 */
static const char *
stringify_adefprivs_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return "COLUMNS";
		case OBJECT_TABLE:
			return "TABLES";
		case OBJECT_SEQUENCE:
			return "SEQUENCES";
		case OBJECT_DATABASE:
			return "DATABASES";
		case OBJECT_DOMAIN:
			return "DOMAINS";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPERS";
		case OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVERS";
		case OBJECT_FUNCTION:
			return "FUNCTIONS";
		case OBJECT_LANGUAGE:
			return "LANGUAGES";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECTS";
		case OBJECT_SCHEMA:
			return "SCHEMAS";
		case OBJECT_PROCEDURE:
			return "PROCEDURES";
		case OBJECT_ROUTINE:
			return "ROUTINES";
		case OBJECT_TABLESPACE:
			return "TABLESPACES";
		case OBJECT_TYPE:
			return "TYPES";
			/* these currently aren't used */
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROLE:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			elog(ERROR, "unsupported object type: %d", (int) objtype);
	}

	return "???";				/* keep compiler quiet */
}
