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
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_proc.c,v 1.115 2004/04/02 23:14:05 tgl Exp $
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
#include "mb/pg_wchar.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* GUC parameter */
bool		check_function_bodies = true;


Datum		fmgr_internal_validator(PG_FUNCTION_ARGS);
Datum		fmgr_c_validator(PG_FUNCTION_ARGS);
Datum		fmgr_sql_validator(PG_FUNCTION_ARGS);

static Datum create_parameternames_array(int parameterCount,
										 const char *parameterNames[]);
static void sql_function_parse_error_callback(void *arg);
static int	match_prosrc_to_query(const char *prosrc, const char *queryText,
								  int cursorpos);
static bool match_prosrc_to_literal(const char *prosrc, const char *literal,
									int cursorpos, int *newcursorpos);


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
				const Oid *parameterTypes,
				const char *parameterNames[])
{
	int			i;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	oldtup;
	char		nulls[Natts_pg_proc];
	Datum		values[Natts_pg_proc];
	char		replaces[Natts_pg_proc];
	Oid			typev[FUNC_MAX_ARGS];
	Datum		namesarray;
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
	 * argument is also ANYARRAY or ANYELEMENT
	 */
	if (returnType == ANYARRAYOID || returnType == ANYELEMENTOID)
	{
		bool		genericParam = false;

		for (i = 0; i < parameterCount; i++)
		{
			if (parameterTypes[i] == ANYARRAYOID ||
				parameterTypes[i] == ANYELEMENTOID)
			{
				genericParam = true;
				break;
			}
		}

		if (!genericParam)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("cannot determine result data type"),
					 errdetail("A function returning \"anyarray\" or \"anyelement\" must have at least one argument of either type.")));
	}

	/* Make sure we have a zero-padded param type array */
	MemSet(typev, 0, FUNC_MAX_ARGS * sizeof(Oid));
	if (parameterCount > 0)
		memcpy(typev, parameterTypes, parameterCount * sizeof(Oid));

	/* Process param names, if given */
	namesarray = create_parameternames_array(parameterCount, parameterNames);

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
	values[i++] = namesarray;					/* proargnames */
	if (namesarray == PointerGetDatum(NULL))
		nulls[Anum_pg_proc_proargnames - 1] = 'n';
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
 * create_parameternames_array - build proargnames value from an array
 * of C strings.  Returns a NULL pointer if no names provided.
 */
static Datum
create_parameternames_array(int parameterCount, const char *parameterNames[])
{
	Datum		elems[FUNC_MAX_ARGS];
	bool		found = false;
	ArrayType  *names;
	int			i;

	if (!parameterNames)
		return PointerGetDatum(NULL);

	for (i=0; i<parameterCount; i++)
	{
		const char *s = parameterNames[i];

		if (s && *s)
			found = true;
		else
			s = "";

		elems[i] = DirectFunctionCall1(textin, CStringGetDatum(s));
	}

	if (!found)
		return PointerGetDatum(NULL);

	names = construct_array(elems, parameterCount, TEXTOID, -1, false, 'i');

	return PointerGetDatum(names);
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
 *
 * The return value is true if the function returns the entire tuple result
 * of its final SELECT, and false otherwise.  Note that because we allow
 * "SELECT rowtype_expression", this may be false even when the declared
 * function return type is a rowtype.
 */
bool
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
		return false;
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
		return false;
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
				return false;	/* NOT returning whole tuple */
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

		/* Report that we are returning entire tuple result */
		return true;
	}
	else if (rettype == RECORDOID)
	{
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
				return false;	/* NOT returning whole tuple */
		}

		/*
		 * Otherwise assume we are returning the whole tuple.  Crosschecking
		 * against what the caller expects will happen at runtime.
		 */
		return true;
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

	return false;
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
	ErrorContextCallback sqlerrcontext;
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
		 * Setup error traceback support for ereport().
		 */
		sqlerrcontext.callback = sql_function_parse_error_callback;
		sqlerrcontext.arg = tuple;
		sqlerrcontext.previous = error_context_stack;
		error_context_stack = &sqlerrcontext;

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
			(void) check_sql_fn_retval(proc->prorettype, functyptype,
									   querytree_list);
		}
		else
			querytree_list = pg_parse_query(prosrc);

		error_context_stack = sqlerrcontext.previous;
	}

	ReleaseSysCache(tuple);

	PG_RETURN_VOID();
}

/*
 * Error context callback for handling errors in SQL function definitions
 */
static void
sql_function_parse_error_callback(void *arg)
{
	HeapTuple	tuple = (HeapTuple) arg;
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(tuple);
	bool		isnull;
	Datum		tmp;
	char	   *prosrc;

	/* See if it's a syntax error; if so, transpose to CREATE FUNCTION */
	tmp = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");
	prosrc = DatumGetCString(DirectFunctionCall1(textout, tmp));

	if (!function_parse_error_transpose(prosrc))
	{
		/* If it's not a syntax error, push info onto context stack */
		errcontext("SQL function \"%s\"", NameStr(proc->proname));
	}

	pfree(prosrc);
}

/*
 * Adjust a syntax error occurring inside the function body of a CREATE
 * FUNCTION command.  This can be used by any function validator, not only
 * for SQL-language functions.  It is assumed that the syntax error position
 * is initially relative to the function body string (as passed in).  If
 * possible, we adjust the position to reference the original CREATE command;
 * if we can't manage that, we set up an "internal query" syntax error instead.
 *
 * Returns true if a syntax error was processed, false if not.
 */
bool
function_parse_error_transpose(const char *prosrc)
{
	int			origerrposition;
	int			newerrposition;
	const char *queryText;

	/*
	 * Nothing to do unless we are dealing with a syntax error that has
	 * a cursor position.
	 *
	 * Some PLs may prefer to report the error position as an internal
	 * error to begin with, so check that too.
	 */
	origerrposition = geterrposition();
	if (origerrposition <= 0)
	{
		origerrposition = getinternalerrposition();
		if (origerrposition <= 0)
			return false;
	}

	/* We can get the original query text from the active portal (hack...) */
	Assert(ActivePortal && ActivePortal->portalActive);
	queryText = ActivePortal->sourceText;

	/* Try to locate the prosrc in the original text */
	newerrposition = match_prosrc_to_query(prosrc, queryText, origerrposition);

	if (newerrposition > 0)
	{
		/* Successful, so fix error position to reference original query */
		errposition(newerrposition);
		/* Get rid of any report of the error as an "internal query" */
		internalerrposition(0);
		internalerrquery(NULL);
	}
	else
	{
		/*
		 * If unsuccessful, convert the position to an internal position
		 * marker and give the function text as the internal query.
		 */
		errposition(0);
		internalerrposition(origerrposition);
		internalerrquery(prosrc);
	}

	return true;
}

/*
 * Try to locate the string literal containing the function body in the
 * given text of the CREATE FUNCTION command.  If successful, return the
 * character (not byte) index within the command corresponding to the
 * given character index within the literal.  If not successful, return 0.
 */
static int
match_prosrc_to_query(const char *prosrc, const char *queryText,
					  int cursorpos)
{
	/*
	 * Rather than fully parsing the CREATE FUNCTION command, we just scan
	 * the command looking for $prosrc$ or 'prosrc'.  This could be fooled
	 * (though not in any very probable scenarios), so fail if we find
	 * more than one match.
	 */
	int		prosrclen = strlen(prosrc);
	int		querylen = strlen(queryText);
	int		matchpos = 0;
	int		curpos;
	int		newcursorpos;

	for (curpos = 0; curpos < querylen-prosrclen; curpos++)
	{
		if (queryText[curpos] == '$' &&
			strncmp(prosrc, &queryText[curpos+1], prosrclen) == 0 &&
			queryText[curpos+1+prosrclen] == '$')
		{
			/*
			 * Found a $foo$ match.  Since there are no embedded quoting
			 * characters in a dollar-quoted literal, we don't have to do
			 * any fancy arithmetic; just offset by the starting position.
			 */
			if (matchpos)
				return 0;		/* multiple matches, fail */
			matchpos = pg_mbstrlen_with_len(queryText, curpos+1)
				+ cursorpos;
		}
		else if (queryText[curpos] == '\'' &&
				 match_prosrc_to_literal(prosrc, &queryText[curpos+1],
										 cursorpos, &newcursorpos))
		{
			/*
			 * Found a 'foo' match.  match_prosrc_to_literal() has adjusted
			 * for any quotes or backslashes embedded in the literal.
			 */
			if (matchpos)
				return 0;		/* multiple matches, fail */
			matchpos = pg_mbstrlen_with_len(queryText, curpos+1)
				+ newcursorpos;
		}
	}

	return matchpos;
}

/*
 * Try to match the given source text to a single-quoted literal.
 * If successful, adjust newcursorpos to correspond to the character
 * (not byte) index corresponding to cursorpos in the source text.
 *
 * At entry, literal points just past a ' character.  We must check for the
 * trailing quote.
 */
static bool
match_prosrc_to_literal(const char *prosrc, const char *literal,
						int cursorpos, int *newcursorpos)
{
	int			newcp = cursorpos;
	int			chlen;

	/*
	 * This implementation handles backslashes and doubled quotes in the
	 * string literal.  It does not handle the SQL syntax for literals
	 * continued across line boundaries.
	 *
	 * We do the comparison a character at a time, not a byte at a time,
	 * so that we can do the correct cursorpos math.
	 */
	while (*prosrc)
	{
		cursorpos--;			/* characters left before cursor */
		/*
		 * Check for backslashes and doubled quotes in the literal; adjust
		 * newcp when one is found before the cursor.
		 */
		if (*literal == '\\')
		{
			literal++;
			if (cursorpos > 0)
				newcp++;
		}
		else if (*literal == '\'')
		{
			if (literal[1] != '\'')
				return false;
			literal++;
			if (cursorpos > 0)
				newcp++;
		}
		chlen = pg_mblen(prosrc);
		if (strncmp(prosrc, literal, chlen) != 0)
			return false;
		prosrc += chlen;
		literal += chlen;
	}

	*newcursorpos = newcp;

	if (*literal == '\'' && literal[1] != '\'')
		return true;
	return false;
}
