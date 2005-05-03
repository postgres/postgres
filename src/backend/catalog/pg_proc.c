/*-------------------------------------------------------------------------
 *
 * pg_proc.c
 *	  routines to support manipulation of the pg_proc relation
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_proc.c,v 1.109.2.1 2005/05/03 16:51:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/sets.h"
#include "utils/syscache.h"


/* GUC parameter */
bool		check_function_bodies = true;


Datum		fmgr_internal_validator(PG_FUNCTION_ARGS);
Datum		fmgr_c_validator(PG_FUNCTION_ARGS);
Datum		fmgr_sql_validator(PG_FUNCTION_ARGS);


/* ----------------------------------------------------------------
 *		ProcedureCreate
 * ----------------------------------------------------------------
 */
Oid
ProcedureCreate(const char *procedureName,
				Oid procNamespace,
				bool replace,
				bool returnsSet,
				Oid returnType,
				Oid languageObjectId,
				Oid languageValidator,
				const char *prosrc,
				const char *probin,
				bool isAgg,
				bool security_definer,
				bool isStrict,
				char volatility,
				int parameterCount,
				const Oid *parameterTypes)
{
	int			i;
	bool		genericParam = false;
	bool		internalParam = false;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	oldtup;
	char		nulls[Natts_pg_proc];
	Datum		values[Natts_pg_proc];
	char		replaces[Natts_pg_proc];
	Oid			typev[FUNC_MAX_ARGS];
	Oid			relid;
	NameData	procname;
	TupleDesc	tupDesc;
	Oid			retval;
	bool		is_update;
	ObjectAddress myself,
				referenced;

	/*
	 * sanity checks
	 */
	Assert(PointerIsValid(prosrc));
	Assert(PointerIsValid(probin));

	if (parameterCount < 0 || parameterCount > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("functions cannot have more than %d arguments",
						FUNC_MAX_ARGS)));

	/*
	 * Do not allow return type ANYARRAY or ANYELEMENT unless at least one
	 * input argument is ANYARRAY or ANYELEMENT.  Also, do not allow
	 * return type INTERNAL unless at least one input argument is INTERNAL.
	 */
	for (i = 0; i < parameterCount; i++)
	{
		switch (parameterTypes[i])
		{
			case ANYARRAYOID:
			case ANYELEMENTOID:
				genericParam = true;
				break;
			case INTERNALOID:
				internalParam = true;
				break;
		}
	}

	if ((returnType == ANYARRAYOID || returnType == ANYELEMENTOID)
		&& !genericParam)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine result data type"),
				 errdetail("A function returning \"anyarray\" or \"anyelement\" must have at least one argument of either type.")));

	if (returnType == INTERNALOID && !internalParam)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("unsafe use of INTERNAL pseudo-type"),
				 errdetail("A function returning \"internal\" must have at least one \"internal\" argument.")));

	/* Make sure we have a zero-padded param type array */
	MemSet(typev, 0, FUNC_MAX_ARGS * sizeof(Oid));
	if (parameterCount > 0)
		memcpy(typev, parameterTypes, parameterCount * sizeof(Oid));

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

			/*
			 * The code below doesn't work any more because the PROSRC
			 * system cache and the pg_proc_prosrc_index have been
			 * removed. Instead a sequential heap scan or something better
			 * must get implemented. The reason for removing is that
			 * nbtree index crashes if sources exceed 2K --- what's likely
			 * for procedural languages.
			 *
			 * 1999/09/30 Jan
			 */
			text	   *prosrctext;

			prosrctext = DatumGetTextP(DirectFunctionCall1(textin,
											   CStringGetDatum(prosrc)));
			retval = GetSysCacheOid(PROSRC,
									PointerGetDatum(prosrctext),
									0, 0, 0);
			pfree(prosrctext);
			if (OidIsValid(retval))
				return retval;
#else
			elog(ERROR, "lookup for procedure by source needs fix (Jan)");
#endif   /* SETS_FIXED */
		}
	}

	/*
	 * don't allow functions of complex types that have the same name as
	 * existing attributes of the type
	 */
	if (parameterCount == 1 && OidIsValid(typev[0]) &&
		(relid = typeidTypeRelid(typev[0])) != InvalidOid &&
		get_attnum(relid, (char *) procedureName) != InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("\"%s\" is already an attribute of type %s",
						procedureName, format_type_be(typev[0]))));

	/*
	 * All seems OK; prepare the data to be inserted into pg_proc.
	 */

	for (i = 0; i < Natts_pg_proc; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;
		replaces[i] = 'r';
	}

	i = 0;
	namestrcpy(&procname, procedureName);
	values[i++] = NameGetDatum(&procname);		/* proname */
	values[i++] = ObjectIdGetDatum(procNamespace);		/* pronamespace */
	values[i++] = Int32GetDatum(GetUserId());	/* proowner */
	values[i++] = ObjectIdGetDatum(languageObjectId);	/* prolang */
	values[i++] = BoolGetDatum(isAgg);	/* proisagg */
	values[i++] = BoolGetDatum(security_definer);		/* prosecdef */
	values[i++] = BoolGetDatum(isStrict);		/* proisstrict */
	values[i++] = BoolGetDatum(returnsSet);		/* proretset */
	values[i++] = CharGetDatum(volatility);		/* provolatile */
	values[i++] = UInt16GetDatum(parameterCount);		/* pronargs */
	values[i++] = ObjectIdGetDatum(returnType); /* prorettype */
	values[i++] = PointerGetDatum(typev);		/* proargtypes */
	values[i++] = DirectFunctionCall1(textin,	/* prosrc */
									  CStringGetDatum(prosrc));
	values[i++] = DirectFunctionCall1(textin,	/* probin */
									  CStringGetDatum(probin));
	/* proacl will be handled below */

	rel = heap_openr(ProcedureRelationName, RowExclusiveLock);
	tupDesc = rel->rd_att;

	/* Check for pre-existing definition */
	oldtup = SearchSysCache(PROCNAMENSP,
							PointerGetDatum(procedureName),
							UInt16GetDatum(parameterCount),
							PointerGetDatum(typev),
							ObjectIdGetDatum(procNamespace));

	if (HeapTupleIsValid(oldtup))
	{
		/* There is one; okay to replace it? */
		Form_pg_proc oldproc = (Form_pg_proc) GETSTRUCT(oldtup);

		if (!replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_FUNCTION),
					 errmsg("function \"%s\" already exists with same argument types",
							procedureName)));
		if (GetUserId() != oldproc->proowner && !superuser())
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
						   procedureName);

		/*
		 * Not okay to change the return type of the existing proc, since
		 * existing rules, views, etc may depend on the return type.
		 */
		if (returnType != oldproc->prorettype ||
			returnsSet != oldproc->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				errmsg("cannot change return type of existing function"),
					 errhint("Use DROP FUNCTION first.")));

		/* Can't change aggregate status, either */
		if (oldproc->proisagg != isAgg)
		{
			if (oldproc->proisagg)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("function \"%s\" is an aggregate",
								procedureName)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("function \"%s\" is not an aggregate",
								procedureName)));
		}

		/* do not change existing ownership or permissions, either */
		replaces[Anum_pg_proc_proowner - 1] = ' ';
		replaces[Anum_pg_proc_proacl - 1] = ' ';

		/* Okay, do it... */
		tup = heap_modifytuple(oldtup, rel, values, nulls, replaces);
		simple_heap_update(rel, &tup->t_self, tup);

		ReleaseSysCache(oldtup);
		is_update = true;
	}
	else
	{
		/* Creating a new procedure */

		/* start out with empty permissions */
		nulls[Anum_pg_proc_proacl - 1] = 'n';

		tup = heap_formtuple(tupDesc, values, nulls);
		simple_heap_insert(rel, tup);
		is_update = false;
	}

	/* Need to update indexes for either the insert or update case */
	CatalogUpdateIndexes(rel, tup);

	retval = HeapTupleGetOid(tup);

	/*
	 * Create dependencies for the new function.  If we are updating an
	 * existing function, first delete any existing pg_depend entries.
	 */
	if (is_update)
		deleteDependencyRecordsFor(RelOid_pg_proc, retval);

	myself.classId = RelOid_pg_proc;
	myself.objectId = retval;
	myself.objectSubId = 0;

	/* dependency on namespace */
	referenced.classId = get_system_catalog_relid(NamespaceRelationName);
	referenced.objectId = procNamespace;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on implementation language */
	referenced.classId = get_system_catalog_relid(LanguageRelationName);
	referenced.objectId = languageObjectId;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on return type */
	referenced.classId = RelOid_pg_type;
	referenced.objectId = returnType;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* dependency on input types */
	for (i = 0; i < parameterCount; i++)
	{
		referenced.classId = RelOid_pg_type;
		referenced.objectId = typev[i];
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);

	/* Verify function body */
	if (OidIsValid(languageValidator))
	{
		/* Advance command counter so new tuple can be seen by validator */
		CommandCounterIncrement();
		OidFunctionCall1(languageValidator, ObjectIdGetDatum(retval));
	}

	return retval;
}

/*
 * check_sql_fn_retval() -- check return value of a list of sql parse trees.
 *
 * The return value of a sql function is the value returned by
 * the final query in the function.  We do some ad-hoc type checking here
 * to be sure that the user is returning the type he claims.
 *
 * This is normally applied during function definition, but in the case
 * of a function with polymorphic arguments, we instead apply it during
 * function execution startup.	The rettype is then the actual resolved
 * output type of the function, rather than the declared type.	(Therefore,
 * we should never see ANYARRAY or ANYELEMENT as rettype.)
 */
void
check_sql_fn_retval(Oid rettype, char fn_typtype, List *queryTreeList)
{
	Query	   *parse;
	int			cmd;
	List	   *tlist;
	List	   *tlistitem;
	int			tlistlen;
	Oid			typerelid;
	Oid			restype;
	Relation	reln;
	int			relnatts;		/* physical number of columns in rel */
	int			rellogcols;		/* # of nondeleted columns in rel */
	int			colindex;		/* physical column index */

	/* guard against empty function body; OK only if void return type */
	if (queryTreeList == NIL)
	{
		if (rettype != VOIDOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
			 errdetail("Function's final statement must be a SELECT.")));
		return;
	}

	/* find the final query */
	parse = (Query *) llast(queryTreeList);

	cmd = parse->commandType;
	tlist = parse->targetList;

	/*
	 * The last query must be a SELECT if and only if return type isn't
	 * VOID.
	 */
	if (rettype == VOIDOID)
	{
		if (cmd == CMD_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
					 errdetail("Function's final statement must not be a SELECT.")));
		return;
	}

	/* by here, the function is declared to return some type */
	if (cmd != CMD_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
		 errmsg("return type mismatch in function declared to return %s",
				format_type_be(rettype)),
			 errdetail("Function's final statement must be a SELECT.")));

	/*
	 * Count the non-junk entries in the result targetlist.
	 */
	tlistlen = ExecCleanTargetListLength(tlist);

	typerelid = typeidTypeRelid(rettype);

	if (fn_typtype == 'b' || fn_typtype == 'd')
	{
		/* Shouldn't have a typerelid */
		Assert(typerelid == InvalidOid);

		/*
		 * For base-type returns, the target list should have exactly one
		 * entry, and its type should agree with what the user declared.
		 * (As of Postgres 7.2, we accept binary-compatible types too.)
		 */
		if (tlistlen != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
			 errdetail("Final SELECT must return exactly one column.")));

		restype = ((TargetEntry *) lfirst(tlist))->resdom->restype;
		if (!IsBinaryCoercible(restype, rettype))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
					 errdetail("Actual return type is %s.",
							   format_type_be(restype))));
	}
	else if (fn_typtype == 'c')
	{
		/* Must have a typerelid */
		Assert(typerelid != InvalidOid);

		/*
		 * If the target list is of length 1, and the type of the varnode
		 * in the target list matches the declared return type, this is
		 * okay. This can happen, for example, where the body of the
		 * function is 'SELECT func2()', where func2 has the same return
		 * type as the function that's calling it.
		 */
		if (tlistlen == 1)
		{
			restype = ((TargetEntry *) lfirst(tlist))->resdom->restype;
			if (IsBinaryCoercible(restype, rettype))
				return;
		}

		/*
		 * Otherwise verify that the targetlist matches the return tuple
		 * type. This part of the typechecking is a hack. We look up the
		 * relation that is the declared return type, and scan the
		 * non-deleted attributes to ensure that they match the datatypes
		 * of the non-resjunk columns.
		 */
		reln = relation_open(typerelid, AccessShareLock);
		relnatts = reln->rd_rel->relnatts;
		rellogcols = 0;			/* we'll count nondeleted cols as we go */
		colindex = 0;

		foreach(tlistitem, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlistitem);
			Form_pg_attribute attr;
			Oid			tletype;
			Oid			atttype;

			if (tle->resdom->resjunk)
				continue;

			do
			{
				colindex++;
				if (colindex > relnatts)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("return type mismatch in function declared to return %s",
									format_type_be(rettype)),
					errdetail("Final SELECT returns too many columns.")));
				attr = reln->rd_att->attrs[colindex - 1];
			} while (attr->attisdropped);
			rellogcols++;

			tletype = exprType((Node *) tle->expr);
			atttype = attr->atttypid;
			if (!IsBinaryCoercible(tletype, atttype))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
						 errdetail("Final SELECT returns %s instead of %s at column %d.",
								   format_type_be(tletype),
								   format_type_be(atttype),
								   rellogcols)));
		}

		for (;;)
		{
			colindex++;
			if (colindex > relnatts)
				break;
			if (!reln->rd_att->attrs[colindex - 1]->attisdropped)
				rellogcols++;
		}

		if (tlistlen != rellogcols)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
					 errdetail("Final SELECT returns too few columns.")));

		relation_close(reln, AccessShareLock);
	}
	else if (rettype == RECORDOID)
	{
		/* Shouldn't have a typerelid */
		Assert(typerelid == InvalidOid);

		/*
		 * For RECORD return type, defer this check until we get the first
		 * tuple.
		 */
	}
	else if (rettype == ANYARRAYOID || rettype == ANYELEMENTOID)
	{
		/* This should already have been caught ... */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("cannot determine result data type"),
				 errdetail("A function returning \"anyarray\" or \"anyelement\" must have at least one argument of either type.")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			  errmsg("return type %s is not supported for SQL functions",
					 format_type_be(rettype))));
}



/*
 * Validator for internal functions
 *
 * Check that the given internal function name (the "prosrc" value) is
 * a known builtin function.
 */
Datum
fmgr_internal_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;

	/*
	 * We do not honor check_function_bodies since it's unlikely the
	 * function name will be found later if it isn't there now.
	 */

	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");
	prosrc = DatumGetCString(DirectFunctionCall1(textout, tmp));

	if (fmgr_internal_function(prosrc) == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("there is no built-in function named \"%s\"",
						prosrc)));

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}



/*
 * Validator for C language functions
 *
 * Make sure that the library file exists, is loadable, and contains
 * the specified link symbol. Also check for a valid function
 * information record.
 */
Datum
fmgr_c_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	void	   *libraryhandle;
	HeapTuple	tuple;
	Form_pg_proc proc;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;
	char	   *probin;

	/*
	 * It'd be most consistent to skip the check if !check_function_bodies,
	 * but the purpose of that switch is to be helpful for pg_dump loading,
	 * and for pg_dump loading it's much better if we *do* check.
	 */

	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");
	prosrc = DatumGetCString(DirectFunctionCall1(textout, tmp));

	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_probin, &isnull);
	if (isnull)
		elog(ERROR, "null probin");
	probin = DatumGetCString(DirectFunctionCall1(textout, tmp));

	(void) load_external_function(probin, prosrc, true, &libraryhandle);
	(void) fetch_finfo_record(libraryhandle, prosrc);

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}


/*
 * Validator for SQL language functions
 *
 * Parse it here in order to be sure that it contains no syntax errors.
 */
Datum
fmgr_sql_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	List	   *querytree_list;
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;
	char		functyptype;
	bool		haspolyarg;
	int			i;

	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for RECORD, VOID, ANYARRAY, or ANYELEMENT */
	if (functyptype == 'p' &&
		proc->prorettype != RECORDOID &&
		proc->prorettype != VOIDOID &&
		proc->prorettype != ANYARRAYOID &&
		proc->prorettype != ANYELEMENTOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("SQL functions cannot return type %s",
						format_type_be(proc->prorettype))));

	/* Disallow pseudotypes in arguments */
	/* except for ANYARRAY or ANYELEMENT */
	haspolyarg = false;
	for (i = 0; i < proc->pronargs; i++)
	{
		if (get_typtype(proc->proargtypes[i]) == 'p')
		{
			if (proc->proargtypes[i] == ANYARRAYOID ||
				proc->proargtypes[i] == ANYELEMENTOID)
				haspolyarg = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("SQL functions cannot have arguments of type %s",
						format_type_be(proc->proargtypes[i]))));
		}
	}

	/* Postpone body checks if !check_function_bodies */
	if (check_function_bodies)
	{
		tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		prosrc = DatumGetCString(DirectFunctionCall1(textout, tmp));

		/*
		 * We can't do full prechecking of the function definition if there
		 * are any polymorphic input types, because actual datatypes of
		 * expression results will be unresolvable.  The check will be done
		 * at runtime instead.
		 *
		 * We can run the text through the raw parser though; this will at
		 * least catch silly syntactic errors.
		 */
		if (!haspolyarg)
		{
			querytree_list = pg_parse_and_rewrite(prosrc,
												  proc->proargtypes,
												  proc->pronargs);
			check_sql_fn_retval(proc->prorettype, functyptype, querytree_list);
		}
		else
			querytree_list = pg_parse_query(prosrc);
	}

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}
