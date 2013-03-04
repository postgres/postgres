/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL EVENT TRIGGER support code.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "utils/syscache.h"
#include "tcop/utility.h"

typedef struct
{
	const char	   *obtypename;
	bool			supported;
} event_trigger_support_data;

typedef enum
{
	EVENT_TRIGGER_COMMAND_TAG_OK,
	EVENT_TRIGGER_COMMAND_TAG_NOT_SUPPORTED,
	EVENT_TRIGGER_COMMAND_TAG_NOT_RECOGNIZED
} event_trigger_command_tag_check_result;

static event_trigger_support_data event_trigger_support[] = {
	{ "AGGREGATE", true },
	{ "CAST", true },
	{ "CONSTRAINT", true },
	{ "COLLATION", true },
	{ "CONVERSION", true },
	{ "DATABASE", false },
	{ "DOMAIN", true },
	{ "EXTENSION", true },
	{ "EVENT TRIGGER", false },
	{ "FOREIGN DATA WRAPPER", true },
	{ "FOREIGN TABLE", true },
	{ "FUNCTION", true },
	{ "INDEX", true },
	{ "LANGUAGE", true },
	{ "MATERIALIZED VIEW", true },
	{ "OPERATOR", true },
	{ "OPERATOR CLASS", true },
	{ "OPERATOR FAMILY", true },
	{ "ROLE", false },
	{ "RULE", true },
	{ "SCHEMA", true },
	{ "SEQUENCE", true },
	{ "SERVER", true },
	{ "TABLE", true },
	{ "TABLESPACE", false},
	{ "TRIGGER", true },
	{ "TEXT SEARCH CONFIGURATION", true },
	{ "TEXT SEARCH DICTIONARY", true },
	{ "TEXT SEARCH PARSER", true },
	{ "TEXT SEARCH TEMPLATE", true },
	{ "TYPE", true },
	{ "USER MAPPING", true },
	{ "VIEW", true },
	{ NULL, false }
};

static void AlterEventTriggerOwner_internal(Relation rel,
											HeapTuple tup,
											Oid newOwnerId);
static event_trigger_command_tag_check_result check_ddl_tag(const char *tag);
static void error_duplicate_filter_variable(const char *defname);
static Datum filter_list_to_array(List *filterlist);
static Oid insert_event_trigger_tuple(char *trigname, char *eventname,
									  Oid evtOwner, Oid funcoid, List *tags);
static void validate_ddl_tags(const char *filtervar, List *taglist);
static void EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata);

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
		strcmp(stmt->eventname, "ddl_command_end") != 0)
		ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("unrecognized event name \"%s\"",
					stmt->eventname)));

	/* Validate filter conditions. */
	foreach (lc, stmt->whenclause)
	{
		DefElem	   *def = (DefElem *) lfirst(lc);

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
	if (strcmp(stmt->eventname, "ddl_command_start") == 0 && tags != NULL)
		validate_ddl_tags("tag", tags);

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
	if (funcrettype != EVTTRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"event_trigger\"",
						NameListToString(stmt->funcname))));

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

	foreach (lc, taglist)
	{
		const char *tag = strVal(lfirst(lc));
		event_trigger_command_tag_check_result result;

		result = check_ddl_tag(tag);
		if (result == EVENT_TRIGGER_COMMAND_TAG_NOT_RECOGNIZED)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("filter value \"%s\" not recognized for filter variable \"%s\"",
						tag, filtervar)));
		if (result == EVENT_TRIGGER_COMMAND_TAG_NOT_SUPPORTED)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /* translator: %s represents an SQL statement name */
				 errmsg("event triggers are not supported for \"%s\"",
					tag)));
	}
}

static event_trigger_command_tag_check_result
check_ddl_tag(const char *tag)
{
	const char *obtypename;
	event_trigger_support_data	   *etsd;

	/*
	 * Handle some idiosyncratic special cases.
	 */
	if (pg_strcasecmp(tag, "CREATE TABLE AS") == 0 ||
		pg_strcasecmp(tag, "SELECT INTO") == 0 ||
		pg_strcasecmp(tag, "REFRESH MATERIALIZED VIEW") == 0 ||
		pg_strcasecmp(tag, "ALTER DEFAULT PRIVILEGES") == 0 ||
		pg_strcasecmp(tag, "ALTER LARGE OBJECT") == 0)
		return EVENT_TRIGGER_COMMAND_TAG_OK;

	/*
	 * Otherwise, command should be CREATE, ALTER, or DROP.
	 */
	if (pg_strncasecmp(tag, "CREATE ", 7) == 0)
		obtypename = tag + 7;
	else if (pg_strncasecmp(tag, "ALTER ", 6) == 0)
		obtypename = tag + 6;
	else if (pg_strncasecmp(tag, "DROP ", 5) == 0)
		obtypename = tag + 5;
	else
		return EVENT_TRIGGER_COMMAND_TAG_NOT_RECOGNIZED;

	/*
	 * ...and the object type should be something recognizable.
	 */
	for (etsd = event_trigger_support; etsd->obtypename != NULL; etsd++)
		if (pg_strcasecmp(etsd->obtypename, obtypename) == 0)
			break;
	if (etsd->obtypename == NULL)
		return EVENT_TRIGGER_COMMAND_TAG_NOT_RECOGNIZED;
	if (!etsd->supported)
		return EVENT_TRIGGER_COMMAND_TAG_NOT_SUPPORTED;
	return EVENT_TRIGGER_COMMAND_TAG_OK;
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
insert_event_trigger_tuple(char *trigname, char *eventname, Oid evtOwner,
						   Oid funcoid, List *taglist)
{
	Relation tgrel;
	Oid         trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	ObjectAddress myself, referenced;

	/* Open pg_event_trigger. */
	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/* Build the new pg_trigger tuple. */
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_event_trigger_evtname - 1] = NameGetDatum(trigname);
	values[Anum_pg_event_trigger_evtevent - 1] = NameGetDatum(eventname);
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
	trigoid = simple_heap_insert(tgrel, tuple);
	CatalogUpdateIndexes(tgrel, tuple);
	heap_freetuple(tuple);

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

	/* Post creation hook for new operator family */
	InvokeObjectAccessHook(OAT_POST_CREATE,
						   EventTriggerRelationId, trigoid, 0, NULL);

	/* Close pg_event_trigger. */
	heap_close(tgrel, RowExclusiveLock);

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

	return PointerGetDatum(construct_array(data, l, TEXTOID, -1, false, 'i'));
}

/*
 * Guts of event trigger deletion.
 */
void
RemoveEventTriggerById(Oid trigOid)
{
	Relation	tgrel;
	HeapTuple	tup;

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCache1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for event trigger %u", trigOid);

	simple_heap_delete(tgrel, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(tgrel, RowExclusiveLock);
}

/*
 * ALTER EVENT TRIGGER foo ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
Oid
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
	Oid         trigoid;
	Form_pg_event_trigger evtForm;
	char        tgenabled = stmt->tgenabled;

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME,
							  CStringGetDatum(stmt->trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist",
					stmt->trigname)));

	trigoid = HeapTupleGetOid(tup);

	if (!pg_event_trigger_ownercheck(trigoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EVENT_TRIGGER,
					   stmt->trigname);

	/* tuple is a copy, so we can modify it below */
	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	evtForm->evtenabled = tgenabled;

	simple_heap_update(tgrel, &tup->t_self, tup);
	CatalogUpdateIndexes(tgrel, tup);

	/* clean up */
	heap_freetuple(tup);
	heap_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
 * Change event trigger's owner -- by name
 */
Oid
AlterEventTriggerOwner(const char *name, Oid newOwnerId)
{
	Oid			evtOid;
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	evtOid = HeapTupleGetOid(tup);

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);

	return evtOid;
}

/*
 * Change extension owner, by OID
 */
void
AlterEventTriggerOwner_oid(Oid trigOid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
		  errmsg("event trigger with OID %u does not exist", trigOid)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);
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

	if (!pg_event_trigger_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EVENT_TRIGGER,
					   NameStr(form->evtname));

	/* New owner must be a superuser */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of event trigger \"%s\"",
					NameStr(form->evtname)),
				 errhint("The owner of an event trigger must be a superuser.")));

	form->evtowner = newOwnerId;
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	/* Update owner dependency reference */
	changeDependencyOnOwner(EventTriggerRelationId,
							HeapTupleGetOid(tup),
							newOwnerId);
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

	oid = GetSysCacheOid1(EVENTTRIGGERNAME, CStringGetDatum(trigname));
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
filter_event_trigger(const char **tag, EventTriggerCacheItem  *item)
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
	if (item->ntags != 0 && bsearch(tag, item->tag,
									item->ntags, sizeof(char *),
									pg_qsort_strcmp) == NULL)
		return false;

	/* if we reach that point, we're not filtering out this item */
	return true;
}

/*
 * Fire ddl_command_start triggers.
 */
void
EventTriggerDDLCommandStart(Node *parsetree)
{
	List	   *cachelist;
	List	   *runlist = NIL;
	ListCell   *lc;
	const char *tag;
	EventTriggerData	trigdata;

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
	 */
	if (!IsUnderPostmaster)
		return;

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
	 * type in question, or you need to adjust check_ddl_tag to accept the
	 * relevant command tag.
	 */
#ifdef USE_ASSERT_CHECKING
	if (assert_enabled)
	{
		const char *dbgtag;

		dbgtag = CreateCommandTag(parsetree);
		if (check_ddl_tag(dbgtag) != EVENT_TRIGGER_COMMAND_TAG_OK)
			elog(ERROR, "unexpected command tag \"%s\"", dbgtag);
	}
#endif

	/* Use cache to find triggers for this event; fast exit if none. */
	cachelist = EventCacheLookup(EVT_DDLCommandStart);
	if (cachelist == NULL)
		return;

	/* Get the command tag. */
	tag = CreateCommandTag(parsetree);

	/*
	 * Filter list of event triggers by command tag, and copy them into
	 * our memory context.  Once we start running the command trigers, or
	 * indeed once we do anything at all that touches the catalogs, an
	 * invalidation might leave cachelist pointing at garbage, so we must
	 * do this before we can do much else.
	 */
	foreach (lc, cachelist)
	{
		EventTriggerCacheItem  *item = lfirst(lc);

		if (filter_event_trigger(&tag, item))
		{
			/* We must plan to fire this trigger. */
			runlist = lappend_oid(runlist, item->fnoid);
		}
	}

	/* Construct event trigger data. */
	trigdata.type = T_EventTriggerData;
	trigdata.event = "ddl_command_start";
	trigdata.parsetree = parsetree;
	trigdata.tag = tag;

	/* Run the triggers. */
	EventTriggerInvoke(runlist, &trigdata);

	/* Cleanup. */
	list_free(runlist);

	/*
	 * Make sure anything the event triggers did will be visible to
	 * the main command.
	 */
	CommandCounterIncrement();
}

/*
 * Fire ddl_command_end triggers.
 */
void
EventTriggerDDLCommandEnd(Node *parsetree)
{
	List	   *cachelist;
	List	   *runlist = NIL;
	ListCell   *lc;
	const char *tag;
	EventTriggerData	trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode.
	 */
	if (!IsUnderPostmaster)
		return;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why this check is
	 * important.
	 *
	 */
#ifdef USE_ASSERT_CHECKING
	if (assert_enabled)
	{
		const char *dbgtag;

		dbgtag = CreateCommandTag(parsetree);
		if (check_ddl_tag(dbgtag) != EVENT_TRIGGER_COMMAND_TAG_OK)
			elog(ERROR, "unexpected command tag \"%s\"", dbgtag);
	}
#endif

	/* Use cache to find triggers for this event; fast exit if none. */
	cachelist = EventCacheLookup(EVT_DDLCommandEnd);
	if (cachelist == NULL)
		return;

	/* Get the command tag. */
	tag = CreateCommandTag(parsetree);

	/*
	 * Filter list of event triggers by command tag, and copy them into
	 * our memory context.  Once we start running the command trigers, or
	 * indeed once we do anything at all that touches the catalogs, an
	 * invalidation might leave cachelist pointing at garbage, so we must
	 * do this before we can do much else.
	 */
	foreach (lc, cachelist)
	{
		EventTriggerCacheItem  *item = lfirst(lc);

		if (filter_event_trigger(&tag, item))
		{
			/* We must plan to fire this trigger. */
			runlist = lappend_oid(runlist, item->fnoid);
		}
	}

	/* Construct event trigger data. */
	trigdata.type = T_EventTriggerData;
	trigdata.event = "ddl_command_end";
	trigdata.parsetree = parsetree;
	trigdata.tag = tag;

	/*
	 * Make sure anything the main command did will be visible to the
	 * event triggers.
	 */
	CommandCounterIncrement();

	/* Run the triggers. */
	EventTriggerInvoke(runlist, &trigdata);

	/* Cleanup. */
	list_free(runlist);
}

/*
 * Invoke each event trigger in a list of event triggers.
 */
static void
EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata)
{
	MemoryContext	context;
	MemoryContext	oldcontext;
	ListCell	   *lc;
	bool			first = true;

	/* Guard against stack overflow due to recursive event trigger */
	check_stack_depth();

	/*
	 * Let's evaluate event triggers in their own memory context, so
	 * that any leaks get cleaned up promptly.
	 */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"event trigger context",
									ALLOCSET_DEFAULT_MINSIZE,
									ALLOCSET_DEFAULT_INITSIZE,
									ALLOCSET_DEFAULT_MAXSIZE);
	oldcontext = MemoryContextSwitchTo(context);

	/* Call each event trigger. */
	foreach (lc, fn_oid_list)
	{
		Oid		fnoid = lfirst_oid(lc);
		FmgrInfo        flinfo;
		FunctionCallInfoData fcinfo;
		PgStat_FunctionCallUsage fcusage;

		/*
		 * We want each event trigger to be able to see the results of
		 * the previous event trigger's action.  Caller is responsible
		 * for any command-counter increment that is needed between the
		 * event trigger and anything else in the transaction.
		 */
		if (first)
			first = false;
		else
			CommandCounterIncrement();

		/* Look up the function */
		fmgr_info(fnoid, &flinfo);

		/* Call the function, passing no arguments but setting a context. */
		InitFunctionCallInfoData(fcinfo, &flinfo, 0,
								 InvalidOid, (Node *) trigdata, NULL);
		pgstat_init_function_usage(&fcinfo, &fcusage);
		FunctionCallInvoke(&fcinfo);
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
 */
bool
EventTriggerSupportsObjectType(ObjectType obtype)
{
	switch (obtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_TABLESPACE:
		case OBJECT_ROLE:
			/* no support for global objects */
			return false;
		case OBJECT_EVENT_TRIGGER:
			/* no support for event triggers on event triggers */
			return false;
		default:
			break;
	}
	return true;
}
