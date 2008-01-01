/*-------------------------------------------------------------------------
 *
 * tsearchcmds.c
 *
 *	  Routines for tsearch manipulation commands
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/tsearchcmds.c,v 1.9 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/heapam.h"
#include "access/genam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void MakeConfigurationMapping(AlterTSConfigurationStmt *stmt,
						 HeapTuple tup, Relation relMap);
static void DropConfigurationMapping(AlterTSConfigurationStmt *stmt,
						 HeapTuple tup, Relation relMap);


/* --------------------- TS Parser commands ------------------------ */

/*
 * lookup a parser support function and return its OID (as a Datum)
 *
 * attnum is the pg_ts_parser column the function will go into
 */
static Datum
get_ts_parser_func(DefElem *defel, int attnum)
{
	List	   *funcName = defGetQualifiedName(defel);
	Oid			typeId[3];
	Oid			retTypeId;
	int			nargs;
	Oid			procOid;

	retTypeId = INTERNALOID;	/* correct for most */
	typeId[0] = INTERNALOID;
	switch (attnum)
	{
		case Anum_pg_ts_parser_prsstart:
			nargs = 2;
			typeId[1] = INT4OID;
			break;
		case Anum_pg_ts_parser_prstoken:
			nargs = 3;
			typeId[1] = INTERNALOID;
			typeId[2] = INTERNALOID;
			break;
		case Anum_pg_ts_parser_prsend:
			nargs = 1;
			retTypeId = VOIDOID;
			break;
		case Anum_pg_ts_parser_prsheadline:
			nargs = 3;
			typeId[1] = INTERNALOID;
			typeId[2] = TSQUERYOID;
			break;
		case Anum_pg_ts_parser_prslextype:
			nargs = 1;
			break;
		default:
			/* should not be here */
			elog(ERROR, "unrecognized attribute for text search parser: %d",
				 attnum);
			nargs = 0;			/* keep compiler quiet */
	}

	procOid = LookupFuncName(funcName, nargs, typeId, false);
	if (get_func_rettype(procOid) != retTypeId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s should return type %s",
						func_signature_string(funcName, nargs, typeId),
						format_type_be(retTypeId))));

	return ObjectIdGetDatum(procOid);
}

/*
 * make pg_depend entries for a new pg_ts_parser entry
 */
static void
makeParserDependencies(HeapTuple tuple)
{
	Form_pg_ts_parser prs = (Form_pg_ts_parser) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;

	myself.classId = TSParserRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/* dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = prs->prsnamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependencies on functions */
	referenced.classId = ProcedureRelationId;
	referenced.objectSubId = 0;

	referenced.objectId = prs->prsstart;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	referenced.objectId = prs->prstoken;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	referenced.objectId = prs->prsend;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	referenced.objectId = prs->prslextype;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (OidIsValid(prs->prsheadline))
	{
		referenced.objectId = prs->prsheadline;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}

/*
 * CREATE TEXT SEARCH PARSER
 */
void
DefineTSParser(List *names, List *parameters)
{
	char	   *prsname;
	ListCell   *pl;
	Relation	prsRel;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_parser];
	char		nulls[Natts_pg_ts_parser];
	NameData	pname;
	Oid			prsOid;
	Oid			namespaceoid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create text search parsers")));

	/* Convert list of names to a name and namespace */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &prsname);

	/* initialize tuple fields with name/namespace */
	memset(values, 0, sizeof(values));
	memset(nulls, ' ', sizeof(nulls));

	namestrcpy(&pname, prsname);
	values[Anum_pg_ts_parser_prsname - 1] = NameGetDatum(&pname);
	values[Anum_pg_ts_parser_prsnamespace - 1] = ObjectIdGetDatum(namespaceoid);

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (pg_strcasecmp(defel->defname, "start") == 0)
		{
			values[Anum_pg_ts_parser_prsstart - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prsstart);
		}
		else if (pg_strcasecmp(defel->defname, "gettoken") == 0)
		{
			values[Anum_pg_ts_parser_prstoken - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prstoken);
		}
		else if (pg_strcasecmp(defel->defname, "end") == 0)
		{
			values[Anum_pg_ts_parser_prsend - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prsend);
		}
		else if (pg_strcasecmp(defel->defname, "headline") == 0)
		{
			values[Anum_pg_ts_parser_prsheadline - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prsheadline);
		}
		else if (pg_strcasecmp(defel->defname, "lextypes") == 0)
		{
			values[Anum_pg_ts_parser_prslextype - 1] =
				get_ts_parser_func(defel, Anum_pg_ts_parser_prslextype);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("text search parser parameter \"%s\" not recognized",
						defel->defname)));
	}

	/*
	 * Validation
	 */
	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prsstart - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser start method is required")));

	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prstoken - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser gettoken method is required")));

	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prsend - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser end method is required")));

	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prslextype - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser lextypes method is required")));

	/*
	 * Looks good, insert
	 */
	prsRel = heap_open(TSParserRelationId, RowExclusiveLock);

	tup = heap_formtuple(prsRel->rd_att, values, nulls);

	prsOid = simple_heap_insert(prsRel, tup);

	CatalogUpdateIndexes(prsRel, tup);

	makeParserDependencies(tup);

	heap_freetuple(tup);

	heap_close(prsRel, RowExclusiveLock);
}

/*
 * DROP TEXT SEARCH PARSER
 */
void
RemoveTSParser(List *names, DropBehavior behavior, bool missing_ok)
{
	Oid			prsOid;
	ObjectAddress object;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop text search parsers")));

	prsOid = TSParserGetPrsid(names, true);
	if (!OidIsValid(prsOid))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("text search parser \"%s\" does not exist",
							NameListToString(names))));
		}
		else
		{
			ereport(NOTICE,
				(errmsg("text search parser \"%s\" does not exist, skipping",
						NameListToString(names))));
		}
		return;
	}

	object.classId = TSParserRelationId;
	object.objectId = prsOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * Guts of TS parser deletion.
 */
void
RemoveTSParserById(Oid prsId)
{
	Relation	relation;
	HeapTuple	tup;

	relation = heap_open(TSParserRelationId, RowExclusiveLock);

	tup = SearchSysCache(TSPARSEROID,
						 ObjectIdGetDatum(prsId),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search parser %u", prsId);

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH PARSER RENAME
 */
void
RenameTSParser(List *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Oid			prsId;
	Oid			namespaceOid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to rename text search parsers")));

	rel = heap_open(TSParserRelationId, RowExclusiveLock);

	prsId = TSParserGetPrsid(oldname, false);

	tup = SearchSysCacheCopy(TSPARSEROID,
							 ObjectIdGetDatum(prsId),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search parser %u", prsId);

	namespaceOid = ((Form_pg_ts_parser) GETSTRUCT(tup))->prsnamespace;

	if (SearchSysCacheExists(TSPARSERNAMENSP,
							 PointerGetDatum(newname),
							 ObjectIdGetDatum(namespaceOid),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("text search parser \"%s\" already exists",
						newname)));

	namestrcpy(&(((Form_pg_ts_parser) GETSTRUCT(tup))->prsname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/* ---------------------- TS Dictionary commands -----------------------*/

/*
 * make pg_depend entries for a new pg_ts_dict entry
 */
static void
makeDictionaryDependencies(HeapTuple tuple)
{
	Form_pg_ts_dict dict = (Form_pg_ts_dict) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;

	myself.classId = TSDictionaryRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/* dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = dict->dictnamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on owner */
	recordDependencyOnOwner(myself.classId, myself.objectId, dict->dictowner);

	/* dependency on template */
	referenced.classId = TSTemplateRelationId;
	referenced.objectId = dict->dicttemplate;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * verify that a template's init method accepts a proposed option list
 */
static void
verify_dictoptions(Oid tmplId, List *dictoptions)
{
	HeapTuple	tup;
	Form_pg_ts_template tform;
	Oid			initmethod;

	/*
	 * Suppress this test when running in a standalone backend.  This is a
	 * hack to allow initdb to create prefab dictionaries that might not
	 * actually be usable in template1's encoding (due to using external files
	 * that can't be translated into template1's encoding).  We want to create
	 * them anyway, since they might be usable later in other databases.
	 */
	if (!IsUnderPostmaster)
		return;

	tup = SearchSysCache(TSTEMPLATEOID,
						 ObjectIdGetDatum(tmplId),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search template %u",
			 tmplId);
	tform = (Form_pg_ts_template) GETSTRUCT(tup);

	initmethod = tform->tmplinit;

	if (!OidIsValid(initmethod))
	{
		/* If there is no init method, disallow any options */
		if (dictoptions)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("text search template \"%s\" does not accept options",
					   NameStr(tform->tmplname))));
	}
	else
	{
		/*
		 * Copy the options just in case init method thinks it can scribble on
		 * them ...
		 */
		dictoptions = copyObject(dictoptions);

		/*
		 * Call the init method and see if it complains.  We don't worry about
		 * it leaking memory, since our command will soon be over anyway.
		 */
		(void) OidFunctionCall1(initmethod, PointerGetDatum(dictoptions));
	}

	ReleaseSysCache(tup);
}

/*
 * CREATE TEXT SEARCH DICTIONARY
 */
void
DefineTSDictionary(List *names, List *parameters)
{
	ListCell   *pl;
	Relation	dictRel;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_dict];
	char		nulls[Natts_pg_ts_dict];
	NameData	dname;
	Oid			templId = InvalidOid;
	List	   *dictoptions = NIL;
	Oid			dictOid;
	Oid			namespaceoid;
	AclResult	aclresult;
	char	   *dictname;

	/* Convert list of names to a name and namespace */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &dictname);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceoid));

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (pg_strcasecmp(defel->defname, "template") == 0)
		{
			templId = TSTemplateGetTmplid(defGetQualifiedName(defel), false);
		}
		else
		{
			/* Assume it's an option for the dictionary itself */
			dictoptions = lappend(dictoptions, defel);
		}
	}

	/*
	 * Validation
	 */
	if (!OidIsValid(templId))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search template is required")));

	verify_dictoptions(templId, dictoptions);

	/*
	 * Looks good, insert
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, ' ', sizeof(nulls));

	namestrcpy(&dname, dictname);
	values[Anum_pg_ts_dict_dictname - 1] = NameGetDatum(&dname);
	values[Anum_pg_ts_dict_dictnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_ts_dict_dictowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_ts_dict_dicttemplate - 1] = ObjectIdGetDatum(templId);
	if (dictoptions)
		values[Anum_pg_ts_dict_dictinitoption - 1] =
			PointerGetDatum(serialize_deflist(dictoptions));
	else
		nulls[Anum_pg_ts_dict_dictinitoption - 1] = 'n';

	dictRel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

	tup = heap_formtuple(dictRel->rd_att, values, nulls);

	dictOid = simple_heap_insert(dictRel, tup);

	CatalogUpdateIndexes(dictRel, tup);

	makeDictionaryDependencies(tup);

	heap_freetuple(tup);

	heap_close(dictRel, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH DICTIONARY RENAME
 */
void
RenameTSDictionary(List *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Oid			dictId;
	Oid			namespaceOid;
	AclResult	aclresult;

	rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

	dictId = TSDictionaryGetDictid(oldname, false);

	tup = SearchSysCacheCopy(TSDICTOID,
							 ObjectIdGetDatum(dictId),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);

	namespaceOid = ((Form_pg_ts_dict) GETSTRUCT(tup))->dictnamespace;

	if (SearchSysCacheExists(TSDICTNAMENSP,
							 PointerGetDatum(newname),
							 ObjectIdGetDatum(namespaceOid),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("text search dictionary \"%s\" already exists",
						newname)));

	/* must be owner */
	if (!pg_ts_dict_ownercheck(dictId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY,
					   NameListToString(oldname));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	namestrcpy(&(((Form_pg_ts_dict) GETSTRUCT(tup))->dictname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * DROP TEXT SEARCH DICTIONARY
 */
void
RemoveTSDictionary(List *names, DropBehavior behavior, bool missing_ok)
{
	Oid			dictOid;
	ObjectAddress object;
	HeapTuple	tup;
	Oid			namespaceId;

	dictOid = TSDictionaryGetDictid(names, true);
	if (!OidIsValid(dictOid))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("text search dictionary \"%s\" does not exist",
							NameListToString(names))));
		}
		else
		{
			ereport(NOTICE,
			(errmsg("text search dictionary \"%s\" does not exist, skipping",
					NameListToString(names))));
		}
		return;
	}

	tup = SearchSysCache(TSDICTOID,
						 ObjectIdGetDatum(dictOid),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictOid);

	/* Permission check: must own dictionary or its namespace */
	namespaceId = ((Form_pg_ts_dict) GETSTRUCT(tup))->dictnamespace;
	if (!pg_ts_dict_ownercheck(dictOid, GetUserId()) &&
		!pg_namespace_ownercheck(namespaceId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY,
					   NameListToString(names));

	ReleaseSysCache(tup);

	object.classId = TSDictionaryRelationId;
	object.objectId = dictOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * Guts of TS dictionary deletion.
 */
void
RemoveTSDictionaryById(Oid dictId)
{
	Relation	relation;
	HeapTuple	tup;

	relation = heap_open(TSDictionaryRelationId, RowExclusiveLock);

	tup = SearchSysCache(TSDICTOID,
						 ObjectIdGetDatum(dictId),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH DICTIONARY
 */
void
AlterTSDictionary(AlterTSDictionaryStmt *stmt)
{
	HeapTuple	tup,
				newtup;
	Relation	rel;
	Oid			dictId;
	ListCell   *pl;
	List	   *dictoptions;
	Datum		opt;
	bool		isnull;
	Datum		repl_val[Natts_pg_ts_dict];
	char		repl_null[Natts_pg_ts_dict];
	char		repl_repl[Natts_pg_ts_dict];

	dictId = TSDictionaryGetDictid(stmt->dictname, false);

	rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

	tup = SearchSysCache(TSDICTOID,
						 ObjectIdGetDatum(dictId),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);

	/* must be owner */
	if (!pg_ts_dict_ownercheck(dictId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY,
					   NameListToString(stmt->dictname));

	/* deserialize the existing set of options */
	opt = SysCacheGetAttr(TSDICTOID, tup,
						  Anum_pg_ts_dict_dictinitoption,
						  &isnull);
	if (isnull)
		dictoptions = NIL;
	else
		dictoptions = deserialize_deflist(opt);

	/*
	 * Modify the options list as per specified changes
	 */
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		ListCell   *cell;
		ListCell   *prev;
		ListCell   *next;

		/*
		 * Remove any matches ...
		 */
		prev = NULL;
		for (cell = list_head(dictoptions); cell; cell = next)
		{
			DefElem    *oldel = (DefElem *) lfirst(cell);

			next = lnext(cell);
			if (pg_strcasecmp(oldel->defname, defel->defname) == 0)
				dictoptions = list_delete_cell(dictoptions, cell, prev);
			else
				prev = cell;
		}

		/*
		 * and add new value if it's got one
		 */
		if (defel->arg)
			dictoptions = lappend(dictoptions, defel);
	}

	/*
	 * Validate
	 */
	verify_dictoptions(((Form_pg_ts_dict) GETSTRUCT(tup))->dicttemplate,
					   dictoptions);

	/*
	 * Looks good, update
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, ' ', sizeof(repl_null));
	memset(repl_repl, ' ', sizeof(repl_repl));

	if (dictoptions)
		repl_val[Anum_pg_ts_dict_dictinitoption - 1] =
			PointerGetDatum(serialize_deflist(dictoptions));
	else
		repl_null[Anum_pg_ts_dict_dictinitoption - 1] = 'n';
	repl_repl[Anum_pg_ts_dict_dictinitoption - 1] = 'r';

	newtup = heap_modifytuple(tup, RelationGetDescr(rel),
							  repl_val, repl_null, repl_repl);

	simple_heap_update(rel, &newtup->t_self, newtup);

	CatalogUpdateIndexes(rel, newtup);

	/*
	 * NOTE: because we only support altering the options, not the template,
	 * there is no need to update dependencies.  This might have to change if
	 * the options ever reference inside-the-database objects.
	 */

	heap_freetuple(newtup);
	ReleaseSysCache(tup);

	heap_close(rel, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH DICTIONARY OWNER
 */
void
AlterTSDictionaryOwner(List *name, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;
	Oid			dictId;
	Oid			namespaceOid;
	AclResult	aclresult;
	Form_pg_ts_dict form;

	rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

	dictId = TSDictionaryGetDictid(name, false);

	tup = SearchSysCacheCopy(TSDICTOID,
							 ObjectIdGetDatum(dictId),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);

	form = (Form_pg_ts_dict) GETSTRUCT(tup);
	namespaceOid = form->dictnamespace;

	if (form->dictowner != newOwnerId)
	{
		/* Superusers can always do it */
		if (!superuser())
		{
			/* must be owner */
			if (!pg_ts_dict_ownercheck(dictId, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY,
							   NameListToString(name));

			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId, ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
							   get_namespace_name(namespaceOid));
		}

		form->dictowner = newOwnerId;

		simple_heap_update(rel, &tup->t_self, tup);
		CatalogUpdateIndexes(rel, tup);

		/* Update owner dependency reference */
		changeDependencyOnOwner(TSDictionaryRelationId, HeapTupleGetOid(tup),
								newOwnerId);
	}

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/* ---------------------- TS Template commands -----------------------*/

/*
 * lookup a template support function and return its OID (as a Datum)
 *
 * attnum is the pg_ts_template column the function will go into
 */
static Datum
get_ts_template_func(DefElem *defel, int attnum)
{
	List	   *funcName = defGetQualifiedName(defel);
	Oid			typeId[4];
	Oid			retTypeId;
	int			nargs;
	Oid			procOid;

	retTypeId = INTERNALOID;
	typeId[0] = INTERNALOID;
	typeId[1] = INTERNALOID;
	typeId[2] = INTERNALOID;
	typeId[3] = INTERNALOID;
	switch (attnum)
	{
		case Anum_pg_ts_template_tmplinit:
			nargs = 1;
			break;
		case Anum_pg_ts_template_tmpllexize:
			nargs = 4;
			break;
		default:
			/* should not be here */
			elog(ERROR, "unrecognized attribute for text search template: %d",
				 attnum);
			nargs = 0;			/* keep compiler quiet */
	}

	procOid = LookupFuncName(funcName, nargs, typeId, false);
	if (get_func_rettype(procOid) != retTypeId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s should return type %s",
						func_signature_string(funcName, nargs, typeId),
						format_type_be(retTypeId))));

	return ObjectIdGetDatum(procOid);
}

/*
 * make pg_depend entries for a new pg_ts_template entry
 */
static void
makeTSTemplateDependencies(HeapTuple tuple)
{
	Form_pg_ts_template tmpl = (Form_pg_ts_template) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;

	myself.classId = TSTemplateRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/* dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = tmpl->tmplnamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependencies on functions */
	referenced.classId = ProcedureRelationId;
	referenced.objectSubId = 0;

	referenced.objectId = tmpl->tmpllexize;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (OidIsValid(tmpl->tmplinit))
	{
		referenced.objectId = tmpl->tmplinit;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}

/*
 * CREATE TEXT SEARCH TEMPLATE
 */
void
DefineTSTemplate(List *names, List *parameters)
{
	ListCell   *pl;
	Relation	tmplRel;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_template];
	char		nulls[Natts_pg_ts_template];
	NameData	dname;
	int			i;
	Oid			dictOid;
	Oid			namespaceoid;
	char	   *tmplname;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			   errmsg("must be superuser to create text search templates")));

	/* Convert list of names to a name and namespace */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &tmplname);

	for (i = 0; i < Natts_pg_ts_template; i++)
	{
		nulls[i] = ' ';
		values[i] = ObjectIdGetDatum(InvalidOid);
	}

	namestrcpy(&dname, tmplname);
	values[Anum_pg_ts_template_tmplname - 1] = NameGetDatum(&dname);
	values[Anum_pg_ts_template_tmplnamespace - 1] = ObjectIdGetDatum(namespaceoid);

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (pg_strcasecmp(defel->defname, "init") == 0)
		{
			values[Anum_pg_ts_template_tmplinit - 1] =
				get_ts_template_func(defel, Anum_pg_ts_template_tmplinit);
			nulls[Anum_pg_ts_template_tmplinit - 1] = ' ';
		}
		else if (pg_strcasecmp(defel->defname, "lexize") == 0)
		{
			values[Anum_pg_ts_template_tmpllexize - 1] =
				get_ts_template_func(defel, Anum_pg_ts_template_tmpllexize);
			nulls[Anum_pg_ts_template_tmpllexize - 1] = ' ';
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
			   errmsg("text search template parameter \"%s\" not recognized",
					  defel->defname)));
	}

	/*
	 * Validation
	 */
	if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_template_tmpllexize - 1])))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search template lexize method is required")));

	/*
	 * Looks good, insert
	 */

	tmplRel = heap_open(TSTemplateRelationId, RowExclusiveLock);

	tup = heap_formtuple(tmplRel->rd_att, values, nulls);

	dictOid = simple_heap_insert(tmplRel, tup);

	CatalogUpdateIndexes(tmplRel, tup);

	makeTSTemplateDependencies(tup);

	heap_freetuple(tup);

	heap_close(tmplRel, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH TEMPLATE RENAME
 */
void
RenameTSTemplate(List *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Oid			tmplId;
	Oid			namespaceOid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			   errmsg("must be superuser to rename text search templates")));

	rel = heap_open(TSTemplateRelationId, RowExclusiveLock);

	tmplId = TSTemplateGetTmplid(oldname, false);

	tup = SearchSysCacheCopy(TSTEMPLATEOID,
							 ObjectIdGetDatum(tmplId),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search template %u",
			 tmplId);

	namespaceOid = ((Form_pg_ts_template) GETSTRUCT(tup))->tmplnamespace;

	if (SearchSysCacheExists(TSTEMPLATENAMENSP,
							 PointerGetDatum(newname),
							 ObjectIdGetDatum(namespaceOid),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("text search template \"%s\" already exists",
						newname)));

	namestrcpy(&(((Form_pg_ts_template) GETSTRUCT(tup))->tmplname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * DROP TEXT SEARCH TEMPLATE
 */
void
RemoveTSTemplate(List *names, DropBehavior behavior, bool missing_ok)
{
	Oid			tmplOid;
	ObjectAddress object;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop text search templates")));

	tmplOid = TSTemplateGetTmplid(names, true);
	if (!OidIsValid(tmplOid))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("text search template \"%s\" does not exist",
							NameListToString(names))));
		}
		else
		{
			ereport(NOTICE,
			  (errmsg("text search template \"%s\" does not exist, skipping",
					  NameListToString(names))));
		}
		return;
	}

	object.classId = TSTemplateRelationId;
	object.objectId = tmplOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * Guts of TS template deletion.
 */
void
RemoveTSTemplateById(Oid tmplId)
{
	Relation	relation;
	HeapTuple	tup;

	relation = heap_open(TSTemplateRelationId, RowExclusiveLock);

	tup = SearchSysCache(TSTEMPLATEOID,
						 ObjectIdGetDatum(tmplId),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search template %u",
			 tmplId);

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}

/* ---------------------- TS Configuration commands -----------------------*/

/*
 * Finds syscache tuple of configuration.
 * Returns NULL if no such cfg.
 */
static HeapTuple
GetTSConfigTuple(List *names)
{
	HeapTuple	tup;
	Oid			cfgId;

	cfgId = TSConfigGetCfgid(names, true);
	if (!OidIsValid(cfgId))
		return NULL;

	tup = SearchSysCache(TSCONFIGOID,
						 ObjectIdGetDatum(cfgId),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgId);

	return tup;
}

/*
 * make pg_depend entries for a new or updated pg_ts_config entry
 *
 * Pass opened pg_ts_config_map relation if there might be any config map
 * entries for the config.
 */
static void
makeConfigurationDependencies(HeapTuple tuple, bool removeOld,
							  Relation mapRel)
{
	Form_pg_ts_config cfg = (Form_pg_ts_config) GETSTRUCT(tuple);
	ObjectAddresses *addrs;
	ObjectAddress myself,
				referenced;

	myself.classId = TSConfigRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/* for ALTER case, first flush old dependencies */
	if (removeOld)
	{
		deleteDependencyRecordsFor(myself.classId, myself.objectId);
		deleteSharedDependencyRecordsFor(myself.classId, myself.objectId);
	}

	/*
	 * We use an ObjectAddresses list to remove possible duplicate
	 * dependencies from the config map info.  The pg_ts_config items
	 * shouldn't be duplicates, but might as well fold them all into one call.
	 */
	addrs = new_object_addresses();

	/* dependency on namespace */
	referenced.classId = NamespaceRelationId;
	referenced.objectId = cfg->cfgnamespace;
	referenced.objectSubId = 0;
	add_exact_object_address(&referenced, addrs);

	/* dependency on owner */
	recordDependencyOnOwner(myself.classId, myself.objectId, cfg->cfgowner);

	/* dependency on parser */
	referenced.classId = TSParserRelationId;
	referenced.objectId = cfg->cfgparser;
	referenced.objectSubId = 0;
	add_exact_object_address(&referenced, addrs);

	/* dependencies on dictionaries listed in config map */
	if (mapRel)
	{
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	maptup;

		/* CCI to ensure we can see effects of caller's changes */
		CommandCounterIncrement();

		ScanKeyInit(&skey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(myself.objectId));

		scan = systable_beginscan(mapRel, TSConfigMapIndexId, true,
								  SnapshotNow, 1, &skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);

			referenced.classId = TSDictionaryRelationId;
			referenced.objectId = cfgmap->mapdict;
			referenced.objectSubId = 0;
			add_exact_object_address(&referenced, addrs);
		}

		systable_endscan(scan);
	}

	/* Record 'em (this includes duplicate elimination) */
	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);

	free_object_addresses(addrs);
}

/*
 * CREATE TEXT SEARCH CONFIGURATION
 */
void
DefineTSConfiguration(List *names, List *parameters)
{
	Relation	cfgRel;
	Relation	mapRel = NULL;
	HeapTuple	tup;
	Datum		values[Natts_pg_ts_config];
	char		nulls[Natts_pg_ts_config];
	AclResult	aclresult;
	Oid			namespaceoid;
	char	   *cfgname;
	NameData	cname;
	Oid			sourceOid = InvalidOid;
	Oid			prsOid = InvalidOid;
	Oid			cfgOid;
	ListCell   *pl;

	/* Convert list of names to a name and namespace */
	namespaceoid = QualifiedNameGetCreationNamespace(names, &cfgname);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(namespaceoid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceoid));

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (pg_strcasecmp(defel->defname, "parser") == 0)
			prsOid = TSParserGetPrsid(defGetQualifiedName(defel), false);
		else if (pg_strcasecmp(defel->defname, "copy") == 0)
			sourceOid = TSConfigGetCfgid(defGetQualifiedName(defel), false);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("text search configuration parameter \"%s\" not recognized",
							defel->defname)));
	}

	if (OidIsValid(sourceOid) && OidIsValid(prsOid))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify both PARSER and COPY options")));

	/*
	 * Look up source config if given.
	 */
	if (OidIsValid(sourceOid))
	{
		Form_pg_ts_config cfg;

		tup = SearchSysCache(TSCONFIGOID,
							 ObjectIdGetDatum(sourceOid),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for text search configuration %u",
				 sourceOid);

		cfg = (Form_pg_ts_config) GETSTRUCT(tup);

		/* use source's parser */
		prsOid = cfg->cfgparser;

		ReleaseSysCache(tup);
	}

	/*
	 * Validation
	 */
	if (!OidIsValid(prsOid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("text search parser is required")));

	/*
	 * Looks good, build tuple and insert
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, ' ', sizeof(nulls));

	namestrcpy(&cname, cfgname);
	values[Anum_pg_ts_config_cfgname - 1] = NameGetDatum(&cname);
	values[Anum_pg_ts_config_cfgnamespace - 1] = ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_ts_config_cfgowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_ts_config_cfgparser - 1] = ObjectIdGetDatum(prsOid);

	cfgRel = heap_open(TSConfigRelationId, RowExclusiveLock);

	tup = heap_formtuple(cfgRel->rd_att, values, nulls);

	cfgOid = simple_heap_insert(cfgRel, tup);

	CatalogUpdateIndexes(cfgRel, tup);

	if (OidIsValid(sourceOid))
	{
		/*
		 * Copy token-dicts map from source config
		 */
		ScanKeyData skey;
		SysScanDesc scan;
		HeapTuple	maptup;

		mapRel = heap_open(TSConfigMapRelationId, RowExclusiveLock);

		ScanKeyInit(&skey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(sourceOid));

		scan = systable_beginscan(mapRel, TSConfigMapIndexId, true,
								  SnapshotNow, 1, &skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);
			HeapTuple	newmaptup;
			Datum		mapvalues[Natts_pg_ts_config_map];
			char		mapnulls[Natts_pg_ts_config_map];

			memset(mapvalues, 0, sizeof(mapvalues));
			memset(mapnulls, ' ', sizeof(mapnulls));

			mapvalues[Anum_pg_ts_config_map_mapcfg - 1] = cfgOid;
			mapvalues[Anum_pg_ts_config_map_maptokentype - 1] = cfgmap->maptokentype;
			mapvalues[Anum_pg_ts_config_map_mapseqno - 1] = cfgmap->mapseqno;
			mapvalues[Anum_pg_ts_config_map_mapdict - 1] = cfgmap->mapdict;

			newmaptup = heap_formtuple(mapRel->rd_att, mapvalues, mapnulls);

			simple_heap_insert(mapRel, newmaptup);

			CatalogUpdateIndexes(mapRel, newmaptup);

			heap_freetuple(newmaptup);
		}

		systable_endscan(scan);
	}

	makeConfigurationDependencies(tup, false, mapRel);

	heap_freetuple(tup);

	if (mapRel)
		heap_close(mapRel, RowExclusiveLock);
	heap_close(cfgRel, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION RENAME
 */
void
RenameTSConfiguration(List *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Oid			cfgId;
	AclResult	aclresult;
	Oid			namespaceOid;

	rel = heap_open(TSConfigRelationId, RowExclusiveLock);

	cfgId = TSConfigGetCfgid(oldname, false);

	tup = SearchSysCacheCopy(TSCONFIGOID,
							 ObjectIdGetDatum(cfgId),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgId);

	namespaceOid = ((Form_pg_ts_config) GETSTRUCT(tup))->cfgnamespace;

	if (SearchSysCacheExists(TSCONFIGNAMENSP,
							 PointerGetDatum(newname),
							 ObjectIdGetDatum(namespaceOid),
							 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("text search configuration \"%s\" already exists",
						newname)));

	/* must be owner */
	if (!pg_ts_config_ownercheck(cfgId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION,
					   NameListToString(oldname));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
				   get_namespace_name(namespaceOid));

	namestrcpy(&(((Form_pg_ts_config) GETSTRUCT(tup))->cfgname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * DROP TEXT SEARCH CONFIGURATION
 */
void
RemoveTSConfiguration(List *names, DropBehavior behavior, bool missing_ok)
{
	Oid			cfgOid;
	Oid			namespaceId;
	ObjectAddress object;
	HeapTuple	tup;

	tup = GetTSConfigTuple(names);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("text search configuration \"%s\" does not exist",
							NameListToString(names))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("text search configuration \"%s\" does not exist, skipping",
							NameListToString(names))));
		}
		return;
	}

	/* Permission check: must own configuration or its namespace */
	cfgOid = HeapTupleGetOid(tup);
	namespaceId = ((Form_pg_ts_config) GETSTRUCT(tup))->cfgnamespace;
	if (!pg_ts_config_ownercheck(cfgOid, GetUserId()) &&
		!pg_namespace_ownercheck(namespaceId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION,
					   NameListToString(names));

	ReleaseSysCache(tup);

	object.classId = TSConfigRelationId;
	object.objectId = cfgOid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}

/*
 * Guts of TS configuration deletion.
 */
void
RemoveTSConfigurationById(Oid cfgId)
{
	Relation	relCfg,
				relMap;
	HeapTuple	tup;
	ScanKeyData skey;
	SysScanDesc scan;

	/* Remove the pg_ts_config entry */
	relCfg = heap_open(TSConfigRelationId, RowExclusiveLock);

	tup = SearchSysCache(TSCONFIGOID,
						 ObjectIdGetDatum(cfgId),
						 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 cfgId);

	simple_heap_delete(relCfg, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relCfg, RowExclusiveLock);

	/* Remove any pg_ts_config_map entries */
	relMap = heap_open(TSConfigMapRelationId, RowExclusiveLock);

	ScanKeyInit(&skey,
				Anum_pg_ts_config_map_mapcfg,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(cfgId));

	scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
							  SnapshotNow, 1, &skey);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		simple_heap_delete(relMap, &tup->t_self);
	}

	systable_endscan(scan);

	heap_close(relMap, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION OWNER
 */
void
AlterTSConfigurationOwner(List *name, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;
	Oid			cfgId;
	AclResult	aclresult;
	Oid			namespaceOid;
	Form_pg_ts_config form;

	rel = heap_open(TSConfigRelationId, RowExclusiveLock);

	cfgId = TSConfigGetCfgid(name, false);

	tup = SearchSysCacheCopy(TSCONFIGOID,
							 ObjectIdGetDatum(cfgId),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgId);

	form = (Form_pg_ts_config) GETSTRUCT(tup);
	namespaceOid = form->cfgnamespace;

	if (form->cfgowner != newOwnerId)
	{
		/* Superusers can always do it */
		if (!superuser())
		{
			/* must be owner */
			if (!pg_ts_config_ownercheck(cfgId, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION,
							   NameListToString(name));

			/* Must be able to become new owner */
			check_is_member_of_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId, ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
							   get_namespace_name(namespaceOid));
		}

		form->cfgowner = newOwnerId;

		simple_heap_update(rel, &tup->t_self, tup);
		CatalogUpdateIndexes(rel, tup);

		/* Update owner dependency reference */
		changeDependencyOnOwner(TSConfigRelationId, HeapTupleGetOid(tup),
								newOwnerId);
	}

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION - main entry point
 */
void
AlterTSConfiguration(AlterTSConfigurationStmt *stmt)
{
	HeapTuple	tup;
	Relation	relMap;

	/* Find the configuration */
	tup = GetTSConfigTuple(stmt->cfgname);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search configuration \"%s\" does not exist",
						NameListToString(stmt->cfgname))));

	/* must be owner */
	if (!pg_ts_config_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION,
					   NameListToString(stmt->cfgname));

	relMap = heap_open(TSConfigMapRelationId, RowExclusiveLock);

	/* Add or drop mappings */
	if (stmt->dicts)
		MakeConfigurationMapping(stmt, tup, relMap);
	else if (stmt->tokentype)
		DropConfigurationMapping(stmt, tup, relMap);

	/* Update dependencies */
	makeConfigurationDependencies(tup, true, relMap);

	heap_close(relMap, RowExclusiveLock);

	ReleaseSysCache(tup);
}

/*
 * Translate a list of token type names to an array of token type numbers
 */
static int *
getTokenTypes(Oid prsId, List *tokennames)
{
	TSParserCacheEntry *prs = lookup_ts_parser_cache(prsId);
	LexDescr   *list;
	int		   *res,
				i,
				ntoken;
	ListCell   *tn;

	ntoken = list_length(tokennames);
	if (ntoken == 0)
		return NULL;
	res = (int *) palloc(sizeof(int) * ntoken);

	if (!OidIsValid(prs->lextypeOid))
		elog(ERROR, "method lextype isn't defined for text search parser %u",
			 prsId);

	/* OidFunctionCall0 is absent */
	list = (LexDescr *) DatumGetPointer(OidFunctionCall1(prs->lextypeOid,
														 (Datum) 0));

	i = 0;
	foreach(tn, tokennames)
	{
		Value	   *val = (Value *) lfirst(tn);
		bool		found = false;
		int			j;

		j = 0;
		while (list && list[j].lexid)
		{
			/* XXX should we use pg_strcasecmp here? */
			if (strcmp(strVal(val), list[j].alias) == 0)
			{
				res[i] = list[j].lexid;
				found = true;
				break;
			}
			j++;
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("token type \"%s\" does not exist",
							strVal(val))));
		i++;
	}

	return res;
}

/*
 * ALTER TEXT SEARCH CONFIGURATION ADD/ALTER MAPPING
 */
static void
MakeConfigurationMapping(AlterTSConfigurationStmt *stmt,
						 HeapTuple tup, Relation relMap)
{
	Oid			cfgId = HeapTupleGetOid(tup);
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	maptup;
	int			i;
	int			j;
	Oid			prsId;
	int		   *tokens,
				ntoken;
	Oid		   *dictIds;
	int			ndict;
	ListCell   *c;

	prsId = ((Form_pg_ts_config) GETSTRUCT(tup))->cfgparser;

	tokens = getTokenTypes(prsId, stmt->tokentype);
	ntoken = list_length(stmt->tokentype);

	if (stmt->override)
	{
		/*
		 * delete maps for tokens if they exist and command was ALTER
		 */
		for (i = 0; i < ntoken; i++)
		{
			ScanKeyInit(&skey[0],
						Anum_pg_ts_config_map_mapcfg,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(cfgId));
			ScanKeyInit(&skey[1],
						Anum_pg_ts_config_map_maptokentype,
						BTEqualStrategyNumber, F_INT4EQ,
						Int32GetDatum(tokens[i]));

			scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
									  SnapshotNow, 2, skey);

			while (HeapTupleIsValid((maptup = systable_getnext(scan))))
			{
				simple_heap_delete(relMap, &maptup->t_self);
			}

			systable_endscan(scan);
		}
	}

	/*
	 * Convert list of dictionary names to array of dict OIDs
	 */
	ndict = list_length(stmt->dicts);
	dictIds = (Oid *) palloc(sizeof(Oid) * ndict);
	i = 0;
	foreach(c, stmt->dicts)
	{
		List	   *names = (List *) lfirst(c);

		dictIds[i] = TSDictionaryGetDictid(names, false);
		i++;
	}

	if (stmt->replace)
	{
		/*
		 * Replace a specific dictionary in existing entries
		 */
		Oid			dictOld = dictIds[0],
					dictNew = dictIds[1];

		ScanKeyInit(&skey[0],
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cfgId));

		scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
								  SnapshotNow, 1, skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);

			/*
			 * check if it's one of target token types
			 */
			if (tokens)
			{
				bool		tokmatch = false;

				for (j = 0; j < ntoken; j++)
				{
					if (cfgmap->maptokentype == tokens[j])
					{
						tokmatch = true;
						break;
					}
				}
				if (!tokmatch)
					continue;
			}

			/*
			 * replace dictionary if match
			 */
			if (cfgmap->mapdict == dictOld)
			{
				Datum		repl_val[Natts_pg_ts_config_map];
				char		repl_null[Natts_pg_ts_config_map];
				char		repl_repl[Natts_pg_ts_config_map];
				HeapTuple	newtup;

				memset(repl_val, 0, sizeof(repl_val));
				memset(repl_null, ' ', sizeof(repl_null));
				memset(repl_repl, ' ', sizeof(repl_repl));

				repl_val[Anum_pg_ts_config_map_mapdict - 1] = ObjectIdGetDatum(dictNew);
				repl_repl[Anum_pg_ts_config_map_mapdict - 1] = 'r';

				newtup = heap_modifytuple(maptup,
										  RelationGetDescr(relMap),
										  repl_val, repl_null, repl_repl);
				simple_heap_update(relMap, &newtup->t_self, newtup);

				CatalogUpdateIndexes(relMap, newtup);
			}
		}

		systable_endscan(scan);
	}
	else
	{
		/*
		 * Insertion of new entries
		 */
		for (i = 0; i < ntoken; i++)
		{
			for (j = 0; j < ndict; j++)
			{
				Datum		values[Natts_pg_ts_config_map];
				char		nulls[Natts_pg_ts_config_map];

				memset(nulls, ' ', sizeof(nulls));
				values[Anum_pg_ts_config_map_mapcfg - 1] = ObjectIdGetDatum(cfgId);
				values[Anum_pg_ts_config_map_maptokentype - 1] = Int32GetDatum(tokens[i]);
				values[Anum_pg_ts_config_map_mapseqno - 1] = Int32GetDatum(j + 1);
				values[Anum_pg_ts_config_map_mapdict - 1] = ObjectIdGetDatum(dictIds[j]);

				tup = heap_formtuple(relMap->rd_att, values, nulls);
				simple_heap_insert(relMap, tup);
				CatalogUpdateIndexes(relMap, tup);

				heap_freetuple(tup);
			}
		}
	}
}

/*
 * ALTER TEXT SEARCH CONFIGURATION DROP MAPPING
 */
static void
DropConfigurationMapping(AlterTSConfigurationStmt *stmt,
						 HeapTuple tup, Relation relMap)
{
	Oid			cfgId = HeapTupleGetOid(tup);
	ScanKeyData skey[2];
	SysScanDesc scan;
	HeapTuple	maptup;
	int			i;
	Oid			prsId;
	int		   *tokens,
				ntoken;
	ListCell   *c;

	prsId = ((Form_pg_ts_config) GETSTRUCT(tup))->cfgparser;

	tokens = getTokenTypes(prsId, stmt->tokentype);
	ntoken = list_length(stmt->tokentype);

	i = 0;
	foreach(c, stmt->tokentype)
	{
		Value	   *val = (Value *) lfirst(c);
		bool		found = false;

		ScanKeyInit(&skey[0],
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cfgId));
		ScanKeyInit(&skey[1],
					Anum_pg_ts_config_map_maptokentype,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(tokens[i]));

		scan = systable_beginscan(relMap, TSConfigMapIndexId, true,
								  SnapshotNow, 2, skey);

		while (HeapTupleIsValid((maptup = systable_getnext(scan))))
		{
			simple_heap_delete(relMap, &maptup->t_self);
			found = true;
		}

		systable_endscan(scan);

		if (!found)
		{
			if (!stmt->missing_ok)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
					   errmsg("mapping for token type \"%s\" does not exist",
							  strVal(val))));
			}
			else
			{
				ereport(NOTICE,
						(errmsg("mapping for token type \"%s\" does not exist, skipping",
								strVal(val))));
			}
		}

		i++;
	}
}


/*
 * Serialize dictionary options, producing a TEXT datum from a List of DefElem
 *
 * This is used to form the value stored in pg_ts_dict.dictinitoption.
 * For the convenience of pg_dump, the output is formatted exactly as it
 * would need to appear in CREATE TEXT SEARCH DICTIONARY to reproduce the
 * same options.
 *
 * Note that we assume that only the textual representation of an option's
 * value is interesting --- hence, non-string DefElems get forced to strings.
 */
text *
serialize_deflist(List *deflist)
{
	text	   *result;
	StringInfoData buf;
	ListCell   *l;

	initStringInfo(&buf);

	foreach(l, deflist)
	{
		DefElem    *defel = (DefElem *) lfirst(l);
		char	   *val = defGetString(defel);

		appendStringInfo(&buf, "%s = ",
						 quote_identifier(defel->defname));
		/* If backslashes appear, force E syntax to determine their handling */
		if (strchr(val, '\\'))
			appendStringInfoChar(&buf, ESCAPE_STRING_SYNTAX);
		appendStringInfoChar(&buf, '\'');
		while (*val)
		{
			char		ch = *val++;

			if (SQL_STR_DOUBLE(ch, true))
				appendStringInfoChar(&buf, ch);
			appendStringInfoChar(&buf, ch);
		}
		appendStringInfoChar(&buf, '\'');
		if (lnext(l) != NULL)
			appendStringInfo(&buf, ", ");
	}

	result = CStringGetTextP(buf.data);
	pfree(buf.data);
	return result;
}

/*
 * Deserialize dictionary options, reconstructing a List of DefElem from TEXT
 *
 * This is also used for prsheadline options, so for backward compatibility
 * we need to accept a few things serialize_deflist() will never emit:
 * in particular, unquoted and double-quoted values.
 */
List *
deserialize_deflist(Datum txt)
{
	text	   *in = DatumGetTextP(txt);		/* in case it's toasted */
	List	   *result = NIL;
	int			len = VARSIZE(in) - VARHDRSZ;
	char	   *ptr,
			   *endptr,
			   *workspace,
			   *wsptr = NULL,
			   *startvalue = NULL;
	typedef enum
	{
		CS_WAITKEY,
		CS_INKEY,
		CS_INQKEY,
		CS_WAITEQ,
		CS_WAITVALUE,
		CS_INSQVALUE,
		CS_INDQVALUE,
		CS_INWVALUE
	} ds_state;
	ds_state	state = CS_WAITKEY;

	workspace = (char *) palloc(len + 1);		/* certainly enough room */
	ptr = VARDATA(in);
	endptr = ptr + len;
	for (; ptr < endptr; ptr++)
	{
		switch (state)
		{
			case CS_WAITKEY:
				if (isspace((unsigned char) *ptr) || *ptr == ',')
					continue;
				if (*ptr == '"')
				{
					wsptr = workspace;
					state = CS_INQKEY;
				}
				else
				{
					wsptr = workspace;
					*wsptr++ = *ptr;
					state = CS_INKEY;
				}
				break;
			case CS_INKEY:
				if (isspace((unsigned char) *ptr))
				{
					*wsptr++ = '\0';
					state = CS_WAITEQ;
				}
				else if (*ptr == '=')
				{
					*wsptr++ = '\0';
					state = CS_WAITVALUE;
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_INQKEY:
				if (*ptr == '"')
				{
					if (ptr + 1 < endptr && ptr[1] == '"')
					{
						/* copy only one of the two quotes */
						*wsptr++ = *ptr++;
					}
					else
					{
						*wsptr++ = '\0';
						state = CS_WAITEQ;
					}
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_WAITEQ:
				if (*ptr == '=')
					state = CS_WAITVALUE;
				else if (!isspace((unsigned char) *ptr))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("invalid parameter list format: \"%s\"",
									TextPGetCString(in))));
				break;
			case CS_WAITVALUE:
				if (*ptr == '\'')
				{
					startvalue = wsptr;
					state = CS_INSQVALUE;
				}
				else if (*ptr == 'E' && ptr + 1 < endptr && ptr[1] == '\'')
				{
					ptr++;
					startvalue = wsptr;
					state = CS_INSQVALUE;
				}
				else if (*ptr == '"')
				{
					startvalue = wsptr;
					state = CS_INDQVALUE;
				}
				else if (!isspace((unsigned char) *ptr))
				{
					startvalue = wsptr;
					*wsptr++ = *ptr;
					state = CS_INWVALUE;
				}
				break;
			case CS_INSQVALUE:
				if (*ptr == '\'')
				{
					if (ptr + 1 < endptr && ptr[1] == '\'')
					{
						/* copy only one of the two quotes */
						*wsptr++ = *ptr++;
					}
					else
					{
						*wsptr++ = '\0';
						result = lappend(result,
										 makeDefElem(pstrdup(workspace),
								  (Node *) makeString(pstrdup(startvalue))));
						state = CS_WAITKEY;
					}
				}
				else if (*ptr == '\\')
				{
					if (ptr + 1 < endptr && ptr[1] == '\\')
					{
						/* copy only one of the two backslashes */
						*wsptr++ = *ptr++;
					}
					else
						*wsptr++ = *ptr;
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_INDQVALUE:
				if (*ptr == '"')
				{
					if (ptr + 1 < endptr && ptr[1] == '"')
					{
						/* copy only one of the two quotes */
						*wsptr++ = *ptr++;
					}
					else
					{
						*wsptr++ = '\0';
						result = lappend(result,
										 makeDefElem(pstrdup(workspace),
								  (Node *) makeString(pstrdup(startvalue))));
						state = CS_WAITKEY;
					}
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			case CS_INWVALUE:
				if (*ptr == ',' || isspace((unsigned char) *ptr))
				{
					*wsptr++ = '\0';
					result = lappend(result,
									 makeDefElem(pstrdup(workspace),
								  (Node *) makeString(pstrdup(startvalue))));
					state = CS_WAITKEY;
				}
				else
				{
					*wsptr++ = *ptr;
				}
				break;
			default:
				elog(ERROR, "unrecognized deserialize_deflist state: %d",
					 state);
		}
	}

	if (state == CS_INWVALUE)
	{
		*wsptr++ = '\0';
		result = lappend(result,
						 makeDefElem(pstrdup(workspace),
								  (Node *) makeString(pstrdup(startvalue))));
	}
	else if (state != CS_WAITKEY)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid parameter list format: \"%s\"",
						TextPGetCString(in))));

	pfree(workspace);

	return result;
}
