/*-------------------------------------------------------------------------
 *
 * pg_proc.c
 *	  routines to support manipulation of the pg_proc relation
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_proc.c,v 1.46 2000/07/05 23:11:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/sets.h"
#include "utils/syscache.h"


/* ----------------------------------------------------------------
 *		ProcedureCreate
 * ----------------------------------------------------------------
 */
Oid
ProcedureCreate(char *procedureName,
				bool returnsSet,
				char *returnTypeName,
				char *languageName,
				char *prosrc,
				char *probin,
				bool trusted,
				bool canCache,
				bool isStrict,
				int32 byte_pct,
				int32 perbyte_cpu,
				int32 percall_cpu,
				int32 outin_ratio,
				List *argList,
				CommandDest dest)
{
	int			i;
	Relation	rel;
	HeapTuple	tup;
	bool		defined;
	uint16		parameterCount;
	char		nulls[Natts_pg_proc];
	Datum		values[Natts_pg_proc];
	Oid			languageObjectId;
	Oid			typeObjectId;
	List	   *x;
	List	   *querytree_list;
	Oid			typev[FUNC_MAX_ARGS];
	Oid			relid;
	Oid			toid;
	NameData	procname;
	TupleDesc	tupDesc;
	Oid		retval;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(PointerIsValid(prosrc));
	Assert(PointerIsValid(probin));

	tup = SearchSysCacheTuple(LANGNAME,
							  PointerGetDatum(languageName),
							  0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "ProcedureCreate: no such language '%s'", languageName);

	languageObjectId = tup->t_data->t_oid;

	parameterCount = 0;
	MemSet(typev, 0, FUNC_MAX_ARGS * sizeof(Oid));
	foreach(x, argList)
	{
		Value	   *t = lfirst(x);

		if (parameterCount >= FUNC_MAX_ARGS)
			elog(ERROR, "Procedures cannot take more than %d arguments",
				 FUNC_MAX_ARGS);

		if (strcmp(strVal(t), "opaque") == 0)
		{
			if (languageObjectId == SQLlanguageId)
				elog(ERROR, "ProcedureCreate: sql functions cannot take type \"opaque\"");
			toid = 0;
		}
		else
		{
			toid = TypeGet(strVal(t), &defined);

			if (!OidIsValid(toid))
			{
				elog(ERROR, "ProcedureCreate: arg type '%s' is not defined",
					 strVal(t));
			}

			if (!defined)
			{
				elog(NOTICE, "ProcedureCreate: arg type '%s' is only a shell",
					 strVal(t));
			}
		}

		typev[parameterCount++] = toid;
	}

	tup = SearchSysCacheTuple(PROCNAME,
							  PointerGetDatum(procedureName),
							  UInt16GetDatum(parameterCount),
							  PointerGetDatum(typev),
							  0);

	if (HeapTupleIsValid(tup))
		elog(ERROR, "ProcedureCreate: procedure %s already exists with same arguments",
			 procedureName);

	if (languageObjectId == SQLlanguageId)
	{

		/*
		 * If this call is defining a set, check if the set is already
		 * defined by looking to see whether this call's function text
		 * matches a function already in pg_proc.  If so just return the
		 * OID of the existing set.
		 */
		if (strcmp(procedureName, GENERICSETNAME) == 0)
		{
#ifdef SETS_FIXED
			/* ----------
			 * The code below doesn't work any more because the
			 * PROSRC system cache and the pg_proc_prosrc_index
			 * have been removed. Instead a sequential heap scan
			 * or something better must get implemented. The reason
			 * for removing is that nbtree index crashes if sources
			 * exceed 2K --- what's likely for procedural languages.
			 *
			 * 1999/09/30 Jan
			 * ----------
			 */
			text	   *prosrctext;

			prosrctext = DatumGetTextP(DirectFunctionCall1(textin,
													CStringGetDatum(prosrc)));
			tup = SearchSysCacheTuple(PROSRC,
									  PointerGetDatum(prosrctext),
									  0, 0, 0);
			pfree(prosrctext);
			if (HeapTupleIsValid(tup))
				return tup->t_data->t_oid;
#else
			elog(ERROR, "lookup for procedure by source needs fix (Jan)");
#endif	 /* SETS_FIXED */
		}
	}

	if (strcmp(returnTypeName, "opaque") == 0)
	{
		if (languageObjectId == SQLlanguageId)
			elog(ERROR, "ProcedureCreate: sql functions cannot return type \"opaque\"");
		typeObjectId = 0;
	}
	else
	{
		typeObjectId = TypeGet(returnTypeName, &defined);

		if (!OidIsValid(typeObjectId))
		{
			elog(NOTICE, "ProcedureCreate: type '%s' is not yet defined",
				 returnTypeName);
			typeObjectId = TypeShellMake(returnTypeName);
			if (!OidIsValid(typeObjectId))
				elog(ERROR, "ProcedureCreate: could not create type '%s'",
					 returnTypeName);
		}
		else if (!defined)
		{
			elog(NOTICE, "ProcedureCreate: return type '%s' is only a shell",
				 returnTypeName);
		}
	}

	/*
	 * don't allow functions of complex types that have the same name as
	 * existing attributes of the type
	 */
	if (parameterCount == 1 &&
		(toid = TypeGet(strVal(lfirst(argList)), &defined)) &&
		defined &&
		(relid = typeidTypeRelid(toid)) != 0 &&
		get_attnum(relid, procedureName) != InvalidAttrNumber)
		elog(ERROR, "method %s already an attribute of type %s",
			 procedureName, strVal(lfirst(argList)));


	/*
	 * If this is a postquel procedure, we parse it here in order to be
	 * sure that it contains no syntax errors.	We should store the plan
	 * in an Inversion file for use later, but for now, we just store the
	 * procedure's text in the prosrc attribute.
	 */

	if (languageObjectId == SQLlanguageId)
	{
		querytree_list = pg_parse_and_rewrite(prosrc, typev, parameterCount);
		/* typecheck return value */
		pg_checkretval(typeObjectId, querytree_list);
	}

	/*
	 * If this is an internal procedure, check that the given internal
	 * function name (the 'prosrc' value) is a known builtin function.
	 *
	 * NOTE: in Postgres versions before 6.5, the SQL name of the created
	 * function could not be different from the internal name, and
	 * 'prosrc' wasn't used.  So there is code out there that does CREATE
	 * FUNCTION xyz AS '' LANGUAGE 'internal'.	To preserve some modicum
	 * of backwards compatibility, accept an empty 'prosrc' value as
	 * meaning the supplied SQL function name.
	 *
	 * XXX: we could treat "internal" and "newinternal" language specs
	 * as equivalent, and take the actual language ID from the table of
	 * known builtin functions.  Is that a better idea than making the
	 * user specify the right thing?  Not sure.
	 */

	if (languageObjectId == INTERNALlanguageId ||
		languageObjectId == NEWINTERNALlanguageId)
	{
		Oid			actualLangID;

		if (strlen(prosrc) == 0)
			prosrc = procedureName;
		actualLangID = fmgr_internal_language(prosrc);
		if (actualLangID == InvalidOid)
			elog(ERROR,
				 "ProcedureCreate: there is no builtin function named \"%s\"",
				 prosrc);
		if (actualLangID != languageObjectId)
			elog(ERROR,
				 "ProcedureCreate: \"%s\" is not %s internal function",
				 prosrc,
				 ((languageObjectId == INTERNALlanguageId) ?
				  "an old-style" : "a new-style"));
	}

	/*
	 * If this is a dynamically loadable procedure, make sure that the
	 * library file exists, is loadable, and contains the specified link
	 * symbol.
	 *
	 * We used to perform these checks only when the function was first
	 * called, but it seems friendlier to verify the library's validity
	 * at CREATE FUNCTION time.
	 */

	if (languageObjectId == ClanguageId ||
		languageObjectId == NEWClanguageId)
	{
		/* If link symbol is specified as "-", substitute procedure name */
		if (strcmp(prosrc, "-") == 0)
			prosrc = procedureName;
		(void) load_external_function(probin, prosrc);
	}

	/*
	 * All seems OK; prepare the tuple to be inserted into pg_proc.
	 */

	for (i = 0; i < Natts_pg_proc; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
	}

	i = 0;
	namestrcpy(&procname, procedureName);
	values[i++] = NameGetDatum(&procname);
	values[i++] = Int32GetDatum(GetUserId());
	values[i++] = ObjectIdGetDatum(languageObjectId);
	/* XXX isinherited is always false for now */
	values[i++] = Int8GetDatum((bool) false);
	values[i++] = Int8GetDatum(trusted);
	values[i++] = Int8GetDatum(canCache);
	values[i++] = Int8GetDatum(isStrict);
	values[i++] = UInt16GetDatum(parameterCount);
	values[i++] = Int8GetDatum(returnsSet);
	values[i++] = ObjectIdGetDatum(typeObjectId);
	values[i++] = (Datum) typev;
	values[i++] = Int32GetDatum(byte_pct);		/* probyte_pct */
	values[i++] = Int32GetDatum(perbyte_cpu);	/* properbyte_cpu */
	values[i++] = Int32GetDatum(percall_cpu);	/* propercall_cpu */
	values[i++] = Int32GetDatum(outin_ratio);	/* prooutin_ratio */
	values[i++] = DirectFunctionCall1(textin,	/* prosrc */
									  CStringGetDatum(prosrc));
	values[i++] = DirectFunctionCall1(textin,	/* probin */
									  CStringGetDatum(probin));

	rel = heap_openr(ProcedureRelationName, RowExclusiveLock);

	tupDesc = rel->rd_att;
	tup = heap_formtuple(tupDesc,
						 values,
						 nulls);

	heap_insert(rel, tup);

	if (RelationGetForm(rel)->relhasindex)
	{
		Relation	idescs[Num_pg_proc_indices];

		CatalogOpenIndices(Num_pg_proc_indices, Name_pg_proc_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_proc_indices, rel, tup);
		CatalogCloseIndices(Num_pg_proc_indices, idescs);
	}
	heap_close(rel, RowExclusiveLock);
	retval = tup->t_data->t_oid;
	heap_freetuple(tup);
	return retval;
}
