/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL PROCEDURAL LANGUAGE support code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/proclang.c,v 1.51.2.2 2005/02/14 06:18:09 tgl Exp $
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
	char	   *languageName;
	Oid			procOid,
				valProcOid;
	Oid			funcrettype;
	Oid			typev[FUNC_MAX_ARGS];
	NameData	langname;
	char		nulls[Natts_pg_language];
	Datum		values[Natts_pg_language];
	Relation	rel;
	HeapTuple	tup;
	TupleDesc	tupDesc;
	int			i;
	ObjectAddress myself,
				referenced;

	/*
	 * Check permission
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("must be superuser to create procedural language")));

	/*
	 * Translate the language name and check that this language doesn't
	 * already exist
	 */
	languageName = case_translate_language_name(stmt->plname);

	if (SearchSysCacheExists(LANGNAME,
							 PointerGetDatum(languageName),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("language \"%s\" already exists", languageName)));

	/*
	 * Lookup the PL handler function and check that it is of the expected
	 * return type
	 */
	MemSet(typev, 0, sizeof(typev));
	procOid = LookupFuncName(stmt->plhandler, 0, typev, false);
	funcrettype = get_func_rettype(procOid);
	if (funcrettype != LANGUAGE_HANDLEROID)
	{
		/*
		 * We allow OPAQUE just so we can load old dump files.	When we
		 * see a handler function declared OPAQUE, change it to
		 * LANGUAGE_HANDLER.
		 */
		if (funcrettype == OPAQUEOID)
		{
			ereport(WARNING,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("changing return type of function %s from \"opaque\" to \"language_handler\"",
							NameListToString(stmt->plhandler))));
			SetFunctionReturnType(procOid, LANGUAGE_HANDLEROID);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("function %s must return type \"language_handler\"",
							NameListToString(stmt->plhandler))));
	}

	/* validate the validator function */
	if (stmt->plvalidator)
	{
		typev[0] = OIDOID;
		valProcOid = LookupFuncName(stmt->plvalidator, 1, typev, false);
		/* return value is ignored, so we don't check the type */
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
	namestrcpy(&langname, languageName);
	values[i++] = NameGetDatum(&langname);			/* lanname */
	values[i++] = BoolGetDatum(true);				/* lanispl */
	values[i++] = BoolGetDatum(stmt->pltrusted);	/* lanpltrusted */
	values[i++] = ObjectIdGetDatum(procOid);		/* lanplcallfoid */
	values[i++] = ObjectIdGetDatum(valProcOid);		/* lanvalidator */
	nulls[i] = 'n';									/* lanacl */

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
	char	   *languageName;
	HeapTuple	langTup;
	ObjectAddress object;

	/*
	 * Check permission
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			   errmsg("must be superuser to drop procedural language")));

	/*
	 * Translate the language name, check that this language exist and is
	 * a PL
	 */
	languageName = case_translate_language_name(stmt->plname);

	langTup = SearchSysCache(LANGNAME,
							 CStringGetDatum(languageName),
							 0, 0, 0);
	if (!HeapTupleIsValid(langTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", languageName)));

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
	if (!HeapTupleIsValid(langTup))		/* should not happen */
		elog(ERROR, "cache lookup failed for language %u", langOid);

	simple_heap_delete(rel, &langTup->t_self);

	ReleaseSysCache(langTup);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Rename language
 */
void
RenameLanguage(const char *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_openr(LanguageRelationName, RowExclusiveLock);

	tup = SearchSysCacheCopy(LANGNAME,
							 CStringGetDatum(oldname),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", oldname)));

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(LANGNAME,
							 CStringGetDatum(newname),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("language \"%s\" already exists", newname)));

	/* must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("must be superuser to rename procedural language")));

	/* rename */
	namestrcpy(&(((Form_pg_language) GETSTRUCT(tup))->lanname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}
