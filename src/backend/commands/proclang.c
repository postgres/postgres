/*-------------------------------------------------------------------------
 *
 * proclang.c--
 *	  PostgreSQL PROCEDURAL LANGUAGE support code.
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_user.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "utils/syscache.h"
#include "commands/proclang.h"
#include "fmgr.h"


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
CreateProceduralLanguage(CreatePLangStmt * stmt)
{
	char		languageName[NAMEDATALEN];
	HeapTuple	langTup;
	HeapTuple	procTup;

	Oid			typev[8];
	char		nulls[Natts_pg_language];
	Datum		values[Natts_pg_language];
	Relation	rdesc;
	HeapTuple	tup;
	TupleDesc	tupDesc;

	int			i;

	/* ----------------
	 * Check permission
	 * ----------------
	 */
	if (!superuser())
	{
		elog(ABORT, "Only users with Postgres superuser privilege are "
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
	{
		elog(ABORT, "Language %s already exists", languageName);
	}

	/* ----------------
	 * Lookup the PL handler function and check that it is
	 * of return type Opaque
	 * ----------------
	 */
	memset(typev, 0, sizeof(typev));
	procTup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(stmt->plhandler),
								  UInt16GetDatum(0),
								  PointerGetDatum(typev),
								  0);
	if (!HeapTupleIsValid(procTup))
	{
		elog(ABORT, "PL handler function %s() doesn't exist",
			 stmt->plhandler);
	}
	if (((Form_pg_proc) GETSTRUCT(procTup))->prorettype != InvalidOid)
	{
		elog(ABORT, "PL handler function %s() isn't of return type Opaque",
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
	values[i++] = ObjectIdGetDatum(procTup->t_oid);
	values[i++] = (Datum) fmgr(TextInRegProcedure, stmt->plcompiler);

	rdesc = heap_openr(LanguageRelationName);

	tupDesc = rdesc->rd_att;
	tup = heap_formtuple(tupDesc, values, nulls);

	heap_insert(rdesc, tup);

	heap_close(rdesc);
	return;
}


/* ---------------------------------------------------------------------
 * DROP PROCEDURAL LANGUAGE
 * ---------------------------------------------------------------------
 */
void
DropProceduralLanguage(DropPLangStmt * stmt)
{
	char		languageName[NAMEDATALEN];
	HeapTuple	langTup;

	Relation	rdesc;
	HeapScanDesc scanDesc;
	ScanKeyData scanKeyData;
	HeapTuple	tup;

	/* ----------------
	 * Check permission
	 * ----------------
	 */
	if (!superuser())
	{
		elog(ABORT, "Only users with Postgres superuser privilege are "
			 "permitted to drop procedural languages");
	}

	/* ----------------
	 * Translate the language name, check that
	 * this language exist and is a PL
	 * ----------------
	 */
	case_translate_language_name(stmt->plname, languageName);

	langTup = SearchSysCacheTuple(LANNAME,
								  PointerGetDatum(languageName),
								  0, 0, 0);
	if (!HeapTupleIsValid(langTup))
	{
		elog(ABORT, "Language %s doesn't exist", languageName);
	}

	if (!((Form_pg_language) GETSTRUCT(langTup))->lanispl)
	{
		elog(ABORT, "Language %s isn't a created procedural language",
			 languageName);
	}

	/* ----------------
	 * Now scan pg_language and delete the PL tuple
	 * ----------------
	 */
	rdesc = heap_openr(LanguageRelationName);

	ScanKeyEntryInitialize(&scanKeyData, 0, Anum_pg_language_lanname,
						   F_NAMEEQ, PointerGetDatum(languageName));

	scanDesc = heap_beginscan(rdesc, 0, false, 1, &scanKeyData);

	tup = heap_getnext(scanDesc, 0, (Buffer *) NULL);

	if (!HeapTupleIsValid(tup))
	{
		elog(ABORT, "Language with name '%s' not found", languageName);
	}

	heap_delete(rdesc, &(tup->t_ctid));

	heap_endscan(scanDesc);
	heap_close(rdesc);
}
