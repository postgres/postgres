/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL PROCEDURAL LANGUAGE support code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/proclang.c,v 1.69 2006/10/04 00:29:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_pltemplate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "miscadmin.h"
#include "parser/gramparse.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef struct
{
	bool		tmpltrusted;	/* trusted? */
	char	   *tmplhandler;	/* name of handler function */
	char	   *tmplvalidator;	/* name of validator function, or NULL */
	char	   *tmpllibrary;	/* path of shared library */
} PLTemplate;

static void create_proc_lang(const char *languageName,
				 Oid handlerOid, Oid valOid, bool trusted);
static PLTemplate *find_language_template(const char *languageName);


/* ---------------------------------------------------------------------
 * CREATE PROCEDURAL LANGUAGE
 * ---------------------------------------------------------------------
 */
void
CreateProceduralLanguage(CreatePLangStmt *stmt)
{
	char	   *languageName;
	PLTemplate *pltemplate;
	Oid			handlerOid,
				valOid;
	Oid			funcrettype;
	Oid			funcargtypes[1];

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
	 * If we have template information for the language, ignore the supplied
	 * parameters (if any) and use the template information.
	 */
	if ((pltemplate = find_language_template(languageName)) != NULL)
	{
		List	   *funcname;

		/*
		 * Give a notice if we are ignoring supplied parameters.
		 */
		if (stmt->plhandler)
			ereport(NOTICE,
					(errmsg("using pg_pltemplate information instead of CREATE LANGUAGE parameters")));

		/*
		 * Find or create the handler function, which we force to be in the
		 * pg_catalog schema.  If already present, it must have the correct
		 * return type.
		 */
		funcname = SystemFuncName(pltemplate->tmplhandler);
		handlerOid = LookupFuncName(funcname, 0, funcargtypes, true);
		if (OidIsValid(handlerOid))
		{
			funcrettype = get_func_rettype(handlerOid);
			if (funcrettype != LANGUAGE_HANDLEROID)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				  errmsg("function %s must return type \"language_handler\"",
						 NameListToString(funcname))));
		}
		else
		{
			handlerOid = ProcedureCreate(pltemplate->tmplhandler,
										 PG_CATALOG_NAMESPACE,
										 false, /* replace */
										 false, /* returnsSet */
										 LANGUAGE_HANDLEROID,
										 ClanguageId,
										 F_FMGR_C_VALIDATOR,
										 pltemplate->tmplhandler,
										 pltemplate->tmpllibrary,
										 false, /* isAgg */
										 false, /* security_definer */
										 false, /* isStrict */
										 PROVOLATILE_VOLATILE,
										 buildoidvector(funcargtypes, 0),
										 PointerGetDatum(NULL),
										 PointerGetDatum(NULL),
										 PointerGetDatum(NULL));
		}

		/*
		 * Likewise for the validator, if required; but we don't care about
		 * its return type.
		 */
		if (pltemplate->tmplvalidator)
		{
			funcname = SystemFuncName(pltemplate->tmplvalidator);
			funcargtypes[0] = OIDOID;
			valOid = LookupFuncName(funcname, 1, funcargtypes, true);
			if (!OidIsValid(valOid))
			{
				valOid = ProcedureCreate(pltemplate->tmplvalidator,
										 PG_CATALOG_NAMESPACE,
										 false, /* replace */
										 false, /* returnsSet */
										 VOIDOID,
										 ClanguageId,
										 F_FMGR_C_VALIDATOR,
										 pltemplate->tmplvalidator,
										 pltemplate->tmpllibrary,
										 false, /* isAgg */
										 false, /* security_definer */
										 false, /* isStrict */
										 PROVOLATILE_VOLATILE,
										 buildoidvector(funcargtypes, 1),
										 PointerGetDatum(NULL),
										 PointerGetDatum(NULL),
										 PointerGetDatum(NULL));
			}
		}
		else
			valOid = InvalidOid;

		/* ok, create it */
		create_proc_lang(languageName, handlerOid, valOid,
						 pltemplate->tmpltrusted);
	}
	else
	{
		/*
		 * No template, so use the provided information.  If there's no
		 * handler clause, the user is trying to rely on a template that we
		 * don't have, so complain accordingly.
		 */
		if (!stmt->plhandler)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unsupported language \"%s\"",
							languageName),
					 errhint("The supported languages are listed in the pg_pltemplate system catalog.")));

		/*
		 * Lookup the PL handler function and check that it is of the expected
		 * return type
		 */
		handlerOid = LookupFuncName(stmt->plhandler, 0, funcargtypes, false);
		funcrettype = get_func_rettype(handlerOid);
		if (funcrettype != LANGUAGE_HANDLEROID)
		{
			/*
			 * We allow OPAQUE just so we can load old dump files.	When we
			 * see a handler function declared OPAQUE, change it to
			 * LANGUAGE_HANDLER.  (This is probably obsolete and removable?)
			 */
			if (funcrettype == OPAQUEOID)
			{
				ereport(WARNING,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("changing return type of function %s from \"opaque\" to \"language_handler\"",
								NameListToString(stmt->plhandler))));
				SetFunctionReturnType(handlerOid, LANGUAGE_HANDLEROID);
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
			funcargtypes[0] = OIDOID;
			valOid = LookupFuncName(stmt->plvalidator, 1, funcargtypes, false);
			/* return value is ignored, so we don't check the type */
		}
		else
			valOid = InvalidOid;

		/* ok, create it */
		create_proc_lang(languageName, handlerOid, valOid, stmt->pltrusted);
	}
}

/*
 * Guts of language creation.
 */
static void
create_proc_lang(const char *languageName,
				 Oid handlerOid, Oid valOid, bool trusted)
{
	Relation	rel;
	TupleDesc	tupDesc;
	Datum		values[Natts_pg_language];
	char		nulls[Natts_pg_language];
	NameData	langname;
	HeapTuple	tup;
	ObjectAddress myself,
				referenced;

	/*
	 * Insert the new language into pg_language
	 */
	rel = heap_open(LanguageRelationId, RowExclusiveLock);
	tupDesc = rel->rd_att;

	memset(values, 0, sizeof(values));
	memset(nulls, ' ', sizeof(nulls));

	namestrcpy(&langname, languageName);
	values[Anum_pg_language_lanname - 1] = NameGetDatum(&langname);
	values[Anum_pg_language_lanispl - 1] = BoolGetDatum(true);
	values[Anum_pg_language_lanpltrusted - 1] = BoolGetDatum(trusted);
	values[Anum_pg_language_lanplcallfoid - 1] = ObjectIdGetDatum(handlerOid);
	values[Anum_pg_language_lanvalidator - 1] = ObjectIdGetDatum(valOid);
	nulls[Anum_pg_language_lanacl - 1] = 'n';

	tup = heap_formtuple(tupDesc, values, nulls);

	simple_heap_insert(rel, tup);

	CatalogUpdateIndexes(rel, tup);

	/*
	 * Create dependencies for language
	 */
	myself.classId = LanguageRelationId;
	myself.objectId = HeapTupleGetOid(tup);
	myself.objectSubId = 0;

	/* dependency on the PL handler function */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = handlerOid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on the validator function, if any */
	if (OidIsValid(valOid))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = valOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	heap_close(rel, RowExclusiveLock);
}

/*
 * Look to see if we have template information for the given language name.
 */
static PLTemplate *
find_language_template(const char *languageName)
{
	PLTemplate *result;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tup;

	rel = heap_open(PLTemplateRelationId, AccessShareLock);

	ScanKeyInit(&key,
				Anum_pg_pltemplate_tmplname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(languageName));
	scan = systable_beginscan(rel, PLTemplateNameIndexId, true,
							  SnapshotNow, 1, &key);

	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		Form_pg_pltemplate tmpl = (Form_pg_pltemplate) GETSTRUCT(tup);
		Datum		datum;
		bool		isnull;

		result = (PLTemplate *) palloc0(sizeof(PLTemplate));
		result->tmpltrusted = tmpl->tmpltrusted;

		/* Remaining fields are variable-width so we need heap_getattr */
		datum = heap_getattr(tup, Anum_pg_pltemplate_tmplhandler,
							 RelationGetDescr(rel), &isnull);
		if (!isnull)
			result->tmplhandler =
				DatumGetCString(DirectFunctionCall1(textout, datum));

		datum = heap_getattr(tup, Anum_pg_pltemplate_tmplvalidator,
							 RelationGetDescr(rel), &isnull);
		if (!isnull)
			result->tmplvalidator =
				DatumGetCString(DirectFunctionCall1(textout, datum));

		datum = heap_getattr(tup, Anum_pg_pltemplate_tmpllibrary,
							 RelationGetDescr(rel), &isnull);
		if (!isnull)
			result->tmpllibrary =
				DatumGetCString(DirectFunctionCall1(textout, datum));

		/* Ignore template if handler or library info is missing */
		if (!result->tmplhandler || !result->tmpllibrary)
			result = NULL;
	}
	else
		result = NULL;

	systable_endscan(scan);

	heap_close(rel, AccessShareLock);

	return result;
}


/*
 * This just returns TRUE if we have a valid template for a given language
 */
bool
PLTemplateExists(const char *languageName)
{
	return (find_language_template(languageName) != NULL);
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
	 * Translate the language name, check that the language exists
	 */
	languageName = case_translate_language_name(stmt->plname);

	langTup = SearchSysCache(LANGNAME,
							 CStringGetDatum(languageName),
							 0, 0, 0);
	if (!HeapTupleIsValid(langTup))
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("language \"%s\" does not exist", languageName)));
		else
			ereport(NOTICE,
					(errmsg("language \"%s\" does not exist, skipping",
							languageName)));

		return;
	}

	object.classId = LanguageRelationId;
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

	rel = heap_open(LanguageRelationId, RowExclusiveLock);

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

	/* Translate both names for consistency with CREATE */
	oldname = case_translate_language_name(oldname);
	newname = case_translate_language_name(newname);

	rel = heap_open(LanguageRelationId, RowExclusiveLock);

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

	/* must be superuser, since we do not have owners for PLs */
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
