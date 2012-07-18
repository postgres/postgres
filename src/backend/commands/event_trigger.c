/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL EVENT TRIGGER support code.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

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
	ObjectType		obtype;
	bool			supported;
} event_trigger_support_data;

static event_trigger_support_data event_trigger_support[] = {
	{ "AGGREGATE", OBJECT_AGGREGATE, true },
	{ "CAST", OBJECT_CAST, true },
	{ "CONSTRAINT", OBJECT_CONSTRAINT, true },
	{ "COLLATION", OBJECT_COLLATION, true },
	{ "CONVERSION", OBJECT_CONVERSION, true },
	{ "DATABASE", OBJECT_DATABASE, false },
	{ "DOMAIN", OBJECT_DOMAIN, true },
	{ "EXTENSION", OBJECT_EXTENSION, true },
	{ "EVENT TRIGGER", OBJECT_EVENT_TRIGGER, false },
	{ "FOREIGN DATA WRAPPER", OBJECT_FDW, true },
	{ "FOREIGN SERVER", OBJECT_FOREIGN_SERVER, true },
	{ "FOREIGN TABLE", OBJECT_FOREIGN_TABLE, true },
	{ "FUNCTION", OBJECT_FUNCTION, true },
	{ "INDEX", OBJECT_INDEX, true },
	{ "LANGUAGE", OBJECT_LANGUAGE, true },
	{ "OPERATOR", OBJECT_OPERATOR, true },
	{ "OPERATOR CLASS", OBJECT_OPCLASS, true },
	{ "OPERATOR FAMILY", OBJECT_OPFAMILY, true },
	{ "ROLE", OBJECT_ROLE, false },
	{ "RULE", OBJECT_RULE, true },
	{ "SCHEMA", OBJECT_SCHEMA, true },
	{ "SEQUENCE", OBJECT_SEQUENCE, true },
	{ "TABLE", OBJECT_TABLE, true },
	{ "TABLESPACE", OBJECT_TABLESPACE, false},
	{ "TRIGGER", OBJECT_TRIGGER, true },
	{ "TEXT SEARCH CONFIGURATION", OBJECT_TSCONFIGURATION, true },
	{ "TEXT SEARCH DICTIONARY", OBJECT_TSDICTIONARY, true },
	{ "TEXT SEARCH PARSER", OBJECT_TSPARSER, true },
	{ "TEXT SEARCH TEMPLATE", OBJECT_TSTEMPLATE, true },
	{ "TYPE", OBJECT_TYPE, true },
	{ "VIEW", OBJECT_VIEW, true },
	{ NULL, (ObjectType) 0, false }
};

static void AlterEventTriggerOwner_internal(Relation rel,
											HeapTuple tup,
											Oid newOwnerId);
static void error_duplicate_filter_variable(const char *defname);
static void error_unrecognized_filter_value(const char *var, const char *val);
static Datum filter_list_to_array(List *filterlist);
static void insert_event_trigger_tuple(char *trigname, char *eventname,
						Oid evtOwner, Oid funcoid, List *tags);
static void validate_ddl_tags(const char *filtervar, List *taglist);

/*
 * Create an event trigger.
 */
void
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
	if (strcmp(stmt->eventname, "ddl_command_start") != 0)
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
	insert_event_trigger_tuple(stmt->trigname, stmt->eventname,
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
		const char *obtypename = NULL;
		event_trigger_support_data	   *etsd;

		/*
		 * As a special case, SELECT INTO is considered DDL, since it creates
		 * a table.
		 */
		if (strcmp(tag, "SELECT INTO") == 0)
			continue;


		/*
		 * Otherwise, it should be CREATE, ALTER, or DROP.
		 */
		if (pg_strncasecmp(tag, "CREATE ", 7) == 0)
			obtypename = tag + 7;
		else if (pg_strncasecmp(tag, "ALTER ", 6) == 0)
			obtypename = tag + 6;
		else if (pg_strncasecmp(tag, "DROP ", 5) == 0)
			obtypename = tag + 5;
		if (obtypename == NULL)
			error_unrecognized_filter_value(filtervar, tag);

		/*
		 * ...and the object type should be something recognizable.
		 */
		for (etsd = event_trigger_support; etsd->obtypename != NULL; etsd++)
			if (pg_strcasecmp(etsd->obtypename, obtypename) == 0)
				break;
		if (etsd->obtypename == NULL)
			error_unrecognized_filter_value(filtervar, tag);
		if (!etsd->supported)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 /* translator: %s represents an SQL statement name */
				 errmsg("event triggers are not supported for \"%s\"",
					tag)));
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
 * Complain about an invalid filter value.
 */
static void
error_unrecognized_filter_value(const char *var, const char *val)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("filter value \"%s\" not recognized for filter variable \"%s\"",
				val, var)));
}

/*
 * Insert the new pg_event_trigger row and record dependencies.
 */
static void
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
void
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
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
	if (!pg_event_trigger_ownercheck(HeapTupleGetOid(tup), GetUserId()))
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
}


/*
 * Rename event trigger
 */
void
RenameEventTrigger(const char *trigname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Form_pg_event_trigger evtForm;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/* newname must be available */
	if (SearchSysCacheExists1(EVENTTRIGGERNAME, CStringGetDatum(newname)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists", newname)));

	/* trigname must exists */
	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", trigname)));
	if (!pg_event_trigger_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EVENT_TRIGGER,
					   trigname);

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);

	/* tuple is a copy, so we can rename it now */
	namestrcpy(&(evtForm->evtname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);
	heap_close(rel, RowExclusiveLock);
}


/*
 * Change event trigger's owner -- by name
 */
void
AlterEventTriggerOwner(const char *name, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);
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
