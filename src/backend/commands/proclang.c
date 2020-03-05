/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL LANGUAGE support code.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/proclang.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * CREATE LANGUAGE
 */
ObjectAddress
CreateProceduralLanguage(CreatePLangStmt *stmt)
{
	const char *languageName = stmt->plname;
	Oid			languageOwner = GetUserId();
	Oid			handlerOid,
				inlineOid,
				valOid;
	Oid			funcrettype;
	Oid			funcargtypes[1];
	Relation	rel;
	TupleDesc	tupDesc;
	Datum		values[Natts_pg_language];
	bool		nulls[Natts_pg_language];
	bool		replaces[Natts_pg_language];
	NameData	langname;
	HeapTuple	oldtup;
	HeapTuple	tup;
	Oid			langoid;
	bool		is_update;
	ObjectAddress myself,
				referenced;

	/*
	 * Check permission
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create custom procedural language")));

	/*
	 * Lookup the PL handler function and check that it is of the expected
	 * return type
	 */
	Assert(stmt->plhandler);
	handlerOid = LookupFuncName(stmt->plhandler, 0, NULL, false);
	funcrettype = get_func_rettype(handlerOid);
	if (funcrettype != LANGUAGE_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->plhandler), "language_handler")));

	/* validate the inline function */
	if (stmt->plinline)
	{
		funcargtypes[0] = INTERNALOID;
		inlineOid = LookupFuncName(stmt->plinline, 1, funcargtypes, false);
		/* return value is ignored, so we don't check the type */
	}
	else
		inlineOid = InvalidOid;

	/* validate the validator function */
	if (stmt->plvalidator)
	{
		funcargtypes[0] = OIDOID;
		valOid = LookupFuncName(stmt->plvalidator, 1, funcargtypes, false);
		/* return value is ignored, so we don't check the type */
	}
	else
		valOid = InvalidOid;

	/* ok to create it */
	rel = table_open(LanguageRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* Prepare data to be inserted */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, true, sizeof(replaces));

	namestrcpy(&langname, languageName);
	values[Anum_pg_language_lanname - 1] = NameGetDatum(&langname);
	values[Anum_pg_language_lanowner - 1] = ObjectIdGetDatum(languageOwner);
	values[Anum_pg_language_lanispl - 1] = BoolGetDatum(true);
	values[Anum_pg_language_lanpltrusted - 1] = BoolGetDatum(stmt->pltrusted);
	values[Anum_pg_language_lanplcallfoid - 1] = ObjectIdGetDatum(handlerOid);
	values[Anum_pg_language_laninline - 1] = ObjectIdGetDatum(inlineOid);
	values[Anum_pg_language_lanvalidator - 1] = ObjectIdGetDatum(valOid);
	nulls[Anum_pg_language_lanacl - 1] = true;

	/* Check for pre-existing definition */
	oldtup = SearchSysCache1(LANGNAME, PointerGetDatum(languageName));

	if (HeapTupleIsValid(oldtup))
	{
		Form_pg_language oldform = (Form_pg_language) GETSTRUCT(oldtup);

		/* There is one; okay to replace it? */
		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("language \"%s\" already exists", languageName)));

		/* This is currently pointless, since we already checked superuser */
#ifdef NOT_USED
		if (!pg_language_ownercheck(oldform->oid, languageOwner))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_LANGUAGE,
						   languageName);
#endif

		/*
		 * Do not change existing oid, ownership or permissions.  Note
		 * dependency-update code below has to agree with this decision.
		 */
		replaces[Anum_pg_language_oid - 1] = false;
		replaces[Anum_pg_language_lanowner - 1] = false;
		replaces[Anum_pg_language_lanacl - 1] = false;

		/* Okay, do it... */
		tup = heap_modify_tuple(oldtup, tupDesc, values, nulls, replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		langoid = oldform->oid;
		ReleaseSysCache(oldtup);
		is_update = true;
	}
	else
	{
		/* Creating a new language */
		langoid = GetNewOidWithIndex(rel, LanguageOidIndexId,
									 Anum_pg_language_oid);
		values[Anum_pg_language_oid - 1] = ObjectIdGetDatum(langoid);
		tup = heap_form_tuple(tupDesc, values, nulls);
		CatalogTupleInsert(rel, tup);
		is_update = false;
	}

	/*
	 * Create dependencies for the new language.  If we are updating an
	 * existing language, first delete any existing pg_depend entries.
	 * (However, since we are not changing ownership or permissions, the
	 * shared dependencies do *not* need to change, and we leave them alone.)
	 */
	myself.classId = LanguageRelationId;
	myself.objectId = langoid;
	myself.objectSubId = 0;

	if (is_update)
		deleteDependencyRecordsFor(myself.classId, myself.objectId, true);

	/* dependency on owner of language */
	if (!is_update)
		recordDependencyOnOwner(myself.classId, myself.objectId,
								languageOwner);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, is_update);

	/* dependency on the PL handler function */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = handlerOid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on the inline handler function, if any */
	if (OidIsValid(inlineOid))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = inlineOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* dependency on the validator function, if any */
	if (OidIsValid(valOid))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = valOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Post creation hook for new procedural language */
	InvokeObjectPostCreateHook(LanguageRelationId, myself.objectId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * Guts of language dropping.
 */
void
DropProceduralLanguageById(Oid langOid)
{
	Relation	rel;
	HeapTuple	langTup;

	rel = table_open(LanguageRelationId, RowExclusiveLock);

	langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(langOid));
	if (!HeapTupleIsValid(langTup)) /* should not happen */
		elog(ERROR, "cache lookup failed for language %u", langOid);

	CatalogTupleDelete(rel, &langTup->t_self);

	ReleaseSysCache(langTup);

	table_close(rel, RowExclusiveLock);
}

/*
 * get_language_oid - given a language name, look up the OID
 *
 * If missing_ok is false, throw an error if language name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_language_oid(const char *langname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(LANGNAME, Anum_pg_language_oid,
						  CStringGetDatum(langname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", langname)));
	return oid;
}
