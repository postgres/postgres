/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL PROCEDURAL LANGUAGE support code.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/proclang.c,v 1.39 2002/08/05 03:29:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/proclang.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* ---------------------------------------------------------------------
 * CREATE PROCEDURAL LANGUAGE
 * ---------------------------------------------------------------------
 */
void
CreateProceduralLanguage(CreatePLangStmt *stmt)
{
	char		languageName[NAMEDATALEN];
	Oid			procOid, valProcOid;
	Oid			typev[FUNC_MAX_ARGS];
	char		nulls[Natts_pg_language];
	Datum		values[Natts_pg_language];
	Relation	rel;
	HeapTuple	tup;
	TupleDesc	tupDesc;
	int			i;
	ObjectAddress	myself,
					referenced;

	/*
	 * Check permission
	 */
	if (!superuser())
		elog(ERROR, "Only users with Postgres superuser privilege are "
			 "permitted to create procedural languages");

	/*
	 * Translate the language name and check that this language doesn't
	 * already exist
	 */
	case_translate_language_name(stmt->plname, languageName);

	if (SearchSysCacheExists(LANGNAME,
							 PointerGetDatum(languageName),
							 0, 0, 0))
		elog(ERROR, "Language %s already exists", languageName);

	/*
	 * Lookup the PL handler function and check that it is of return type
	 * Opaque
	 */
	MemSet(typev, 0, sizeof(typev));
	procOid = LookupFuncName(stmt->plhandler, 0, typev);
	if (!OidIsValid(procOid))
		elog(ERROR, "PL handler function %s() doesn't exist",
			 NameListToString(stmt->plhandler));
	if (get_func_rettype(procOid) != InvalidOid)
		elog(ERROR, "PL handler function %s() does not return type \"opaque\"",
			 NameListToString(stmt->plhandler));

	/* validate the validator function */
	if (stmt->plvalidator)
	{
		typev[0] = OIDOID;
		valProcOid = LookupFuncName(stmt->plvalidator, 1, typev);
		if (!OidIsValid(valProcOid))
			elog(ERROR, "PL validator function %s(oid) doesn't exist",
				 NameListToString(stmt->plvalidator));
	}
	else
		valProcOid = InvalidOid;

	/*
	 * Insert the new language into pg_language
	 */
	for (i = 0; i < Natts_pg_language; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}

	i = 0;
	values[i++] = PointerGetDatum(languageName);
	values[i++] = BoolGetDatum(true);	/* lanispl */
	values[i++] = BoolGetDatum(stmt->pltrusted);
	values[i++] = ObjectIdGetDatum(procOid);
	values[i++] = ObjectIdGetDatum(valProcOid);
	nulls[i] = 'n';				/* lanacl */

	rel = heap_openr(LanguageRelationName, RowExclusiveLock);

	tupDesc = rel->rd_att;
	tup = heap_formtuple(tupDesc, values, nulls);

	simple_heap_insert(rel, tup);

	CatalogUpdateIndexes(rel, tup);

	/*
	 * Create dependencies for language
	 */
	myself.classId = RelationGetRelid(rel);
	myself.objectId = HeapTupleGetOid(tup);
	myself.objectSubId = 0;

	/* dependency on the PL handler function */
	referenced.classId = RelOid_pg_proc;
	referenced.objectId = procOid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on the validator function, if any */
	if (OidIsValid(valProcOid))
	{
		referenced.classId = RelOid_pg_proc;
		referenced.objectId = valProcOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	heap_close(rel, RowExclusiveLock);
}


/* ---------------------------------------------------------------------
 * DROP PROCEDURAL LANGUAGE
 * ---------------------------------------------------------------------
 */
void
DropProceduralLanguage(DropPLangStmt *stmt)
{
	char		languageName[NAMEDATALEN];
	HeapTuple	langTup;
	ObjectAddress object;

	/*
	 * Check permission
	 */
	if (!superuser())
		elog(ERROR, "Only users with Postgres superuser privilege are "
			 "permitted to drop procedural languages");

	/*
	 * Translate the language name, check that this language exist and is
	 * a PL
	 */
	case_translate_language_name(stmt->plname, languageName);

	langTup = SearchSysCache(LANGNAME,
							 CStringGetDatum(languageName),
							 0, 0, 0);
	if (!HeapTupleIsValid(langTup))
		elog(ERROR, "Language %s doesn't exist", languageName);

	if (!((Form_pg_language) GETSTRUCT(langTup))->lanispl)
		elog(ERROR, "Language %s isn't a created procedural language",
			 languageName);

	object.classId = get_system_catalog_relid(LanguageRelationName);
	object.objectId = HeapTupleGetOid(langTup);
	object.objectSubId = 0;

	ReleaseSysCache(langTup);

	/*
	 * Do the deletion
	 */
	performDeletion(&object, stmt->behavior);
}

/*
 * Guts of language dropping.
 */
void
DropProceduralLanguageById(Oid langOid)
{
	Relation	rel;
	HeapTuple	langTup;

	rel = heap_openr(LanguageRelationName, RowExclusiveLock);

	langTup = SearchSysCache(LANGOID,
							 ObjectIdGetDatum(langOid),
							 0, 0, 0);
	if (!HeapTupleIsValid(langTup))
		elog(ERROR, "DropProceduralLanguageById: language %u not found",
			 langOid);

	simple_heap_delete(rel, &langTup->t_self);

	ReleaseSysCache(langTup);

	heap_close(rel, RowExclusiveLock);
}
