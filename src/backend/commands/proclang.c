/*-------------------------------------------------------------------------
 *
 * proclang.c
 *	  PostgreSQL PROCEDURAL LANGUAGE support code.
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "commands/proclang.h"
#include "fmgr.h"
#include "utils/syscache.h"


static void
case_translate_language_name(const char *input, char *output)
{
/*-------------------------------------------------------------------------
  Translate the input language name to lower case, except if it's C,
  translate to upper case.
--------------------------------------------------------------------------*/
	int			i;

	for (i = 0; i < NAMEDATALEN && input[i]; ++i)
		output[i] = tolower(input[i]);

	output[i] = '\0';

	if (strcmp(output, "c") == 0)
		output[0] = 'C';
}


/* ---------------------------------------------------------------------
 * CREATE PROCEDURAL LANGUAGE
 * ---------------------------------------------------------------------
 */
void
CreateProceduralLanguage(CreatePLangStmt *stmt)
{
	char		languageName[NAMEDATALEN];
	HeapTuple	langTup;
	HeapTuple	procTup;

	Oid			typev[8];
	char		nulls[Natts_pg_language];
	Datum		values[Natts_pg_language];
	Relation	rel;
	HeapTuple	tup;
	TupleDesc	tupDesc;

	int			i;

	/* ----------------
	 * Check permission
	 * ----------------
	 */
	if (!superuser())
	{
		elog(ERROR, "Only users with Postgres superuser privilege are "
			 "permitted to create procedural languages");
	}

	/* ----------------
	 * Translate the language name and check that
	 * this language doesn't already exist
	 * ----------------
	 */
	case_translate_language_name(stmt->plname, languageName);

	langTup = SearchSysCacheTuple(LANNAME,
								  PointerGetDatum(languageName),
								  0, 0, 0);
	if (HeapTupleIsValid(langTup))
		elog(ERROR, "Language %s already exists", languageName);

	/* ----------------
	 * Lookup the PL handler function and check that it is
	 * of return type Opaque
	 * ----------------
	 */
	memset(typev, 0, sizeof(typev));
	procTup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(stmt->plhandler),
								  Int32GetDatum(0),
								  PointerGetDatum(typev),
								  0);
	if (!HeapTupleIsValid(procTup))
	{
		elog(ERROR, "PL handler function %s() doesn't exist",
			 stmt->plhandler);
	}
	if (((Form_pg_proc) GETSTRUCT(procTup))->prorettype != InvalidOid)
	{
		elog(ERROR, "PL handler function %s() isn't of return type Opaque",
			 stmt->plhandler);
	}

	/* ----------------
	 * Insert the new language into pg_language
	 * ----------------
	 */
	for (i = 0; i < Natts_pg_language; i++)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}

	i = 0;
	values[i++] = PointerGetDatum(languageName);
	values[i++] = Int8GetDatum((bool) 1);
	values[i++] = Int8GetDatum(stmt->pltrusted);
	values[i++] = ObjectIdGetDatum(procTup->t_data->t_oid);
	values[i++] = (Datum) fmgr(F_TEXTIN, stmt->plcompiler);

	rel = heap_openr(LanguageRelationName, RowExclusiveLock);

	tupDesc = rel->rd_att;
	tup = heap_formtuple(tupDesc, values, nulls);

	heap_insert(rel, tup);

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
	Relation	rel;

	/* ----------------
	 * Check permission
	 * ----------------
	 */
	if (!superuser())
	{
		elog(ERROR, "Only users with Postgres superuser privilege are "
			 "permitted to drop procedural languages");
	}

	/* ----------------
	 * Translate the language name, check that
	 * this language exist and is a PL
	 * ----------------
	 */
	case_translate_language_name(stmt->plname, languageName);

	rel = heap_openr(LanguageRelationName, RowExclusiveLock);

	langTup = SearchSysCacheTupleCopy(LANNAME,
									  PointerGetDatum(languageName),
									  0, 0, 0);
	if (!HeapTupleIsValid(langTup))
		elog(ERROR, "Language %s doesn't exist", languageName);

	if (!((Form_pg_language) GETSTRUCT(langTup))->lanispl)
		elog(ERROR, "Language %s isn't a created procedural language",
			 languageName);

	heap_delete(rel, &langTup->t_self, NULL);

	pfree(langTup);
	heap_close(rel, RowExclusiveLock);
}
