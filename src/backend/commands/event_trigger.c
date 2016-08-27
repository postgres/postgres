/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL EVENT TRIGGER support code.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "tcop/deparse_utility.h"
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

typedef struct EventTriggerQueryState
{
	/* memory context for this state's objects */
	MemoryContext cxt;

	/* sql_drop */
	slist_head	SQLDropList;
	bool		in_sql_drop;

	/* table_rewrite */
	Oid			table_rewrite_oid;		/* InvalidOid, or set for
										 * table_rewrite event */
	int			table_rewrite_reason;	/* AT_REWRITE reason */

	/* Support for command collection */
	bool		commandCollectionInhibited;
	CollectedCommand *currentCommand;
	List	   *commandList;	/* list of CollectedCommand; see
								 * deparse_utility.h */
	struct EventTriggerQueryState *previous;
} EventTriggerQueryState;

static EventTriggerQueryState *currentEventTriggerState = NULL;

typedef struct
{
	const char *obtypename;
	bool		supported;
} event_trigger_support_data;

typedef enum
{
	EVENT_TRIGGER_COMMAND_TAG_OK,
	EVENT_TRIGGER_COMMAND_TAG_NOT_SUPPORTED,
	EVENT_TRIGGER_COMMAND_TAG_NOT_RECOGNIZED
} event_trigger_command_tag_check_result;

/* XXX merge this with ObjectTypeMap? */
static event_trigger_support_data event_trigger_support[] = {
	{"ACCESS METHOD", true},
	{"AGGREGATE", true},
	{"CAST", true},
	{"CONSTRAINT", true},
	{"COLLATION", true},
	{"CONVERSION", true},
	{"DATABASE", false},
	{"DOMAIN", true},
	{"EXTENSION", true},
	{"EVENT TRIGGER", false},
	{"FOREIGN DATA WRAPPER", true},
	{"FOREIGN TABLE", true},
	{"FUNCTION", true},
	{"INDEX", true},
	{"LANGUAGE", true},
	{"MATERIALIZED VIEW", true},
	{"OPERATOR", true},
	{"OPERATOR CLASS", true},
	{"OPERATOR FAMILY", true},
	{"POLICY", true},
	{"ROLE", false},
	{"RULE", true},
	{"SCHEMA", true},
	{"SEQUENCE", true},
	{"SERVER", true},
	{"TABLE", true},
	{"TABLESPACE", false},
	{"TRANSFORM", true},
	{"TRIGGER", true},
	{"TEXT SEARCH CONFIGURATION", true},
	{"TEXT SEARCH DICTIONARY", true},
	{"TEXT SEARCH PARSER", true},
	{"TEXT SEARCH TEMPLATE", true},
	{"TYPE", true},
	{"USER MAPPING", true},
	{"VIEW", true},
	{NULL, false}
};

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
static event_trigger_command_tag_check_result check_ddl_tag(const char *tag);
static event_trigger_command_tag_check_result check_table_rewrite_ddl_tag(
							const char *tag);
static void error_duplicate_filter_variable(const char *defname);
static Datum filter_list_to_array(List *filterlist);
static Oid insert_event_trigger_tuple(char *trigname, char *eventname,
						   Oid evtOwner, Oid funcoid, List *tags);
static void validate_ddl_tags(const char *filtervar, List *taglist);
static void validate_table_rewrite_tags(const char *filtervar, List *taglist);
static void EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata);
static const char *stringify_grantobjtype(GrantObjectType objtype);
static const char *stringify_adefprivs_objtype(GrantObjectType objtype);

/*
 * Create an event trigger.
 */
Oid
CreateEventTrigger(CreateEventTrigStmt *stmt)
{
	HeapTuple	tuple;
	Oid			funcoid;
	Oid			funcrettype;
	Oid			fargtypes[1];	/* dummy */
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
	funcoid = LookupFuncName(stmt->funcname, 0, fargtypes, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != EVTTRIGGEROID)
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
					 errmsg("event triggers are not supported for %s",
							tag)));
	}
}

static event_trigger_command_tag_check_result
check_ddl_tag(const char *tag)
{
	const char *obtypename;
	event_trigger_support_data *etsd;

	/*
	 * Handle some idiosyncratic special cases.
	 */
	if (pg_strcasecmp(tag, "CREATE TABLE AS") == 0 ||
		pg_strcasecmp(tag, "SELECT INTO") == 0 ||
		pg_strcasecmp(tag, "REFRESH MATERIALIZED VIEW") == 0 ||
		pg_strcasecmp(tag, "ALTER DEFAULT PRIVILEGES") == 0 ||
		pg_strcasecmp(tag, "ALTER LARGE OBJECT") == 0 ||
		pg_strcasecmp(tag, "COMMENT") == 0 ||
		pg_strcasecmp(tag, "GRANT") == 0 ||
		pg_strcasecmp(tag, "REVOKE") == 0 ||
		pg_strcasecmp(tag, "DROP OWNED") == 0 ||
		pg_strcasecmp(tag, "IMPORT FOREIGN SCHEMA") == 0 ||
		pg_strcasecmp(tag, "SECURITY LABEL") == 0)
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
 * Validate DDL command tags for event table_rewrite.
 */
static void
validate_table_rewrite_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tag = strVal(lfirst(lc));
		event_trigger_command_tag_check_result result;

		result = check_table_rewrite_ddl_tag(tag);
		if (result == EVENT_TRIGGER_COMMAND_TAG_NOT_SUPPORTED)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
					 errmsg("event triggers are not supported for %s",
							tag)));
	}
}

static event_trigger_command_tag_check_result
check_table_rewrite_ddl_tag(const char *tag)
{
	if (pg_strcasecmp(tag, "ALTER TABLE") == 0 ||
		pg_strcasecmp(tag, "ALTER TYPE") == 0)
		return EVENT_TRIGGER_COMMAND_TAG_OK;

	return EVENT_TRIGGER_COMMAND_TAG_NOT_SUPPORTED;
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
	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/* Build the new pg_trigger tuple. */
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

	/* Depend on extension, if any. */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new event trigger */
	InvokeObjectPostCreateHook(EventTriggerRelationId, trigoid, 0);

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
	Oid			trigoid;
	Form_pg_event_trigger evtForm;
	char		tgenabled = stmt->tgenabled;

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

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  trigoid, 0);

	/* clean up */
	heap_freetuple(tup);
	heap_close(tgrel, RowExclusiveLock);

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
	Relation	rel;
	ObjectAddress address;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	evtOid = HeapTupleGetOid(tup);

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, EventTriggerRelationId, evtOid);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);

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

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  HeapTupleGetOid(tup), 0);
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
filter_event_trigger(const char **tag, EventTriggerCacheItem *item)
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
 * Setup for running triggers for the given event.  Return value is an OID list
 * of functions to run; if there are any, trigdata is filled with an
 * appropriate EventTriggerData for them to receive.
 */
static List *
EventTriggerCommonSetup(Node *parsetree,
						EventTriggerEvent event, const char *eventstr,
						EventTriggerData *trigdata)
{
	const char *tag;
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
	 * type in question, or you need to adjust check_ddl_tag to accept the
	 * relevant command tag.
	 */
#ifdef USE_ASSERT_CHECKING
	{
		const char *dbgtag;

		dbgtag = CreateCommandTag(parsetree);
		if (event == EVT_DDLCommandStart ||
			event == EVT_DDLCommandEnd ||
			event == EVT_SQLDrop)
		{
			if (check_ddl_tag(dbgtag) != EVENT_TRIGGER_COMMAND_TAG_OK)
				elog(ERROR, "unexpected command tag \"%s\"", dbgtag);
		}
		else if (event == EVT_TableRewrite)
		{
			if (check_table_rewrite_ddl_tag(dbgtag) != EVENT_TRIGGER_COMMAND_TAG_OK)
				elog(ERROR, "unexpected command tag \"%s\"", dbgtag);
		}
	}
#endif

	/* Use cache to find triggers for this event; fast exit if none. */
	cachelist = EventCacheLookup(event);
	if (cachelist == NIL)
		return NIL;

	/* Get the command tag. */
	tag = CreateCommandTag(parsetree);

	/*
	 * Filter list of event triggers by command tag, and copy them into our
	 * memory context.  Once we start running the command trigers, or indeed
	 * once we do anything at all that touches the catalogs, an invalidation
	 * might leave cachelist pointing at garbage, so we must do this before we
	 * can do much else.
	 */
	foreach(lc, cachelist)
	{
		EventTriggerCacheItem *item = lfirst(lc);

		if (filter_event_trigger(&tag, item))
		{
			/* We must plan to fire this trigger. */
			runlist = lappend_oid(runlist, item->fnoid);
		}
	}

	/* don't spend any more time on this if no functions to run */
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
	 */
	if (!IsUnderPostmaster)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandStart,
									  "ddl_command_start",
									  &trigdata);
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
	 * triggers are disabled in single user mode.
	 */
	if (!IsUnderPostmaster)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandEnd, "ddl_command_end",
									  &trigdata);
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
	 * triggers are disabled in single user mode.
	 */
	if (!IsUnderPostmaster)
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
									  &trigdata);

	/*
	 * Nothing to do if run list is empty.  Note this shouldn't happen,
	 * because if there are no sql_drop events, then objects-to-drop wouldn't
	 * have been collected in the first place and we would have quit above.
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
	PG_CATCH();
	{
		currentEventTriggerState->in_sql_drop = false;
		PG_RE_THROW();
	}
	PG_END_TRY();
	currentEventTriggerState->in_sql_drop = false;

	/* Cleanup. */
	list_free(runlist);
}


/*
 * Fire table_rewrite triggers.
 */
void
EventTriggerTableRewrite(Node *parsetree, Oid tableOid, int reason)
{
	List	   *runlist;
	EventTriggerData trigdata;

	elog(DEBUG1, "EventTriggerTableRewrite(%u)", tableOid);

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

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_TableRewrite,
									  "table_rewrite",
									  &trigdata);
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
	PG_CATCH();
	{
		currentEventTriggerState->table_rewrite_oid = InvalidOid;
		currentEventTriggerState->table_rewrite_reason = 0;
		PG_RE_THROW();
	}
	PG_END_TRY();

	currentEventTriggerState->table_rewrite_oid = InvalidOid;
	currentEventTriggerState->table_rewrite_reason = 0;

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
		Oid			fnoid = lfirst_oid(lc);
		FmgrInfo	flinfo;
		FunctionCallInfoData fcinfo;
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
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLUMN:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFACL:
		case OBJECT_DEFAULT:
		case OBJECT_DOMAIN:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EXTENSION:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_FUNCTION:
		case OBJECT_INDEX:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_POLICY:
		case OBJECT_RULE:
		case OBJECT_SCHEMA:
		case OBJECT_SEQUENCE:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TABLE:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_TYPE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			return true;
	}
	return true;
}

/*
 * Do event triggers support this object class?
 */
bool
EventTriggerSupportsObjectClass(ObjectClass objclass)
{
	switch (objclass)
	{
		case OCLASS_DATABASE:
		case OCLASS_TBLSPACE:
		case OCLASS_ROLE:
			/* no support for global objects */
			return false;
		case OCLASS_EVENT_TRIGGER:
			/* no support for event triggers on event triggers */
			return false;
		case OCLASS_CLASS:
		case OCLASS_PROC:
		case OCLASS_TYPE:
		case OCLASS_CAST:
		case OCLASS_COLLATION:
		case OCLASS_CONSTRAINT:
		case OCLASS_CONVERSION:
		case OCLASS_DEFAULT:
		case OCLASS_LANGUAGE:
		case OCLASS_LARGEOBJECT:
		case OCLASS_OPERATOR:
		case OCLASS_OPCLASS:
		case OCLASS_OPFAMILY:
		case OCLASS_AMOP:
		case OCLASS_AMPROC:
		case OCLASS_REWRITE:
		case OCLASS_TRIGGER:
		case OCLASS_SCHEMA:
		case OCLASS_TRANSFORM:
		case OCLASS_TSPARSER:
		case OCLASS_TSDICT:
		case OCLASS_TSTEMPLATE:
		case OCLASS_TSCONFIG:
		case OCLASS_FDW:
		case OCLASS_FOREIGN_SERVER:
		case OCLASS_USER_MAPPING:
		case OCLASS_DEFACL:
		case OCLASS_EXTENSION:
		case OCLASS_POLICY:
		case OCLASS_AM:
			return true;
	}

	return true;
}

bool
EventTriggerSupportsGrantObjectType(GrantObjectType objtype)
{
	switch (objtype)
	{
		case ACL_OBJECT_DATABASE:
		case ACL_OBJECT_TABLESPACE:
			/* no support for global objects */
			return false;

		case ACL_OBJECT_COLUMN:
		case ACL_OBJECT_RELATION:
		case ACL_OBJECT_SEQUENCE:
		case ACL_OBJECT_DOMAIN:
		case ACL_OBJECT_FDW:
		case ACL_OBJECT_FOREIGN_SERVER:
		case ACL_OBJECT_FUNCTION:
		case ACL_OBJECT_LANGUAGE:
		case ACL_OBJECT_LARGEOBJECT:
		case ACL_OBJECT_NAMESPACE:
		case ACL_OBJECT_TYPE:
			return true;
		default:
			Assert(false);
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
	return list_length(EventCacheLookup(EVT_SQLDrop)) > 0 ||
		list_length(EventCacheLookup(EVT_TableRewrite)) > 0 ||
		list_length(EventCacheLookup(EVT_DDLCommandEnd)) > 0;
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

	Assert(EventTriggerSupportsObjectClass(getObjectClass(object)));

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

		catalog = heap_open(obj->address.classId, AccessShareLock);
		tuple = get_catalog_object_by_oid(catalog, obj->address.objectId);

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
						heap_close(catalog, AccessShareLock);
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

		heap_close(catalog, AccessShareLock);
	}
	else
	{
		if (object->classId == NamespaceRelationId &&
			isTempNamespace(object->objectId))
			obj->istemp = true;
	}

	/* object identity, objname and objargs */
	obj->objidentity =
		getObjectIdentityParts(&obj->address, &obj->addrnames, &obj->addrargs);

	/* object type */
	obj->objecttype = getObjectTypeDescription(&obj->address);

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
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
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

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Build tuplestore to hold the result rows */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	slist_foreach(iter, &(currentEventTriggerState->SQLDropList))
	{
		SQLDropObject *obj;
		int			i = 0;
		Datum		values[12];
		bool		nulls[12];

		obj = slist_container(SQLDropObject, next, iter.cur);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

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

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

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
 *
 * XXX -- this API isn't considering the possibility of an ALTER TABLE command
 * being called reentrantly by an event trigger function.  Do we need stackable
 * commands at this level?	Perhaps at least we should detect the condition and
 * raise an error.
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
	/* ignore if event trigger context not set, or collection disabled */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	/* If no subcommands, don't collect */
	if (list_length(currentEventTriggerState->currentCommand->d.alterTable.subcmds) != 0)
	{
		currentEventTriggerState->commandList =
			lappend(currentEventTriggerState->commandList,
					currentEventTriggerState->currentCommand);
	}
	else
		pfree(currentEventTriggerState->currentCommand);

	currentEventTriggerState->currentCommand = NULL;
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
	command->parsetree = copyObject(stmt);

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
	command->parsetree = copyObject(stmt);

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
	command->parsetree = copyObject(stmt);

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
	command->parsetree = copyObject(stmt);

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
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	ListCell   *lc;

	/*
	 * Protect this function from being called out of context
	 */
	if (!currentEventTriggerState)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in an event trigger function",
						"pg_event_trigger_ddl_commands()")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Build tuplestore to hold the result rows */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	foreach(lc, currentEventTriggerState->commandList)
	{
		CollectedCommand *cmd = lfirst(lc);
		Datum		values[9];
		bool		nulls[9];
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

		MemSet(nulls, 0, sizeof(nulls));

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

					type = getObjectTypeDescription(&addr);
					identity = getObjectIdentity(&addr);

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

							catalog = heap_open(addr.classId, AccessShareLock);
							objtup = get_catalog_object_by_oid(catalog,
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
							/* XXX not quite get_namespace_name_or_temp */
							if (isAnyTempNamespace(schema_oid))
								schema = pstrdup("pg_temp");
							else
								schema = get_namespace_name(schema_oid);

							heap_close(catalog, AccessShareLock);
						}
					}

					/* classid */
					values[i++] = ObjectIdGetDatum(addr.classId);
					/* objid */
					values[i++] = ObjectIdGetDatum(addr.objectId);
					/* objsubid */
					values[i++] = Int32GetDatum(addr.objectSubId);
					/* command tag */
					values[i++] = CStringGetTextDatum(CreateCommandTag(cmd->parsetree));
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
				values[i++] = CStringGetTextDatum(CreateCommandTag(cmd->parsetree));
				/* object_type */
				values[i++] = CStringGetTextDatum(stringify_adefprivs_objtype(
												   cmd->d.defprivs.objtype));
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
				values[i++] = CStringGetTextDatum(stringify_grantobjtype(
											   cmd->d.grant.istmt->objtype));
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

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/*
 * Return the GrantObjectType as a string, as it would appear in GRANT and
 * REVOKE commands.
 */
static const char *
stringify_grantobjtype(GrantObjectType objtype)
{
	switch (objtype)
	{
		case ACL_OBJECT_COLUMN:
			return "COLUMN";
		case ACL_OBJECT_RELATION:
			return "TABLE";
		case ACL_OBJECT_SEQUENCE:
			return "SEQUENCE";
		case ACL_OBJECT_DATABASE:
			return "DATABASE";
		case ACL_OBJECT_DOMAIN:
			return "DOMAIN";
		case ACL_OBJECT_FDW:
			return "FOREIGN DATA WRAPPER";
		case ACL_OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVER";
		case ACL_OBJECT_FUNCTION:
			return "FUNCTION";
		case ACL_OBJECT_LANGUAGE:
			return "LANGUAGE";
		case ACL_OBJECT_LARGEOBJECT:
			return "LARGE OBJECT";
		case ACL_OBJECT_NAMESPACE:
			return "SCHEMA";
		case ACL_OBJECT_TABLESPACE:
			return "TABLESPACE";
		case ACL_OBJECT_TYPE:
			return "TYPE";
		default:
			elog(ERROR, "unrecognized type %d", objtype);
			return "???";		/* keep compiler quiet */
	}
}

/*
 * Return the GrantObjectType as a string; as above, but use the spelling
 * in ALTER DEFAULT PRIVILEGES commands instead.
 */
static const char *
stringify_adefprivs_objtype(GrantObjectType objtype)
{
	switch (objtype)
	{
		case ACL_OBJECT_RELATION:
			return "TABLES";
			break;
		case ACL_OBJECT_FUNCTION:
			return "FUNCTIONS";
			break;
		case ACL_OBJECT_SEQUENCE:
			return "SEQUENCES";
			break;
		case ACL_OBJECT_TYPE:
			return "TYPES";
			break;
		default:
			elog(ERROR, "unrecognized type %d", objtype);
			return "???";		/* keep compiler quiet */
	}
}
