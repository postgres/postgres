/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_func.c,v 1.119 2002/03/21 16:01:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "nodes/makefuncs.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static Node *ParseComplexProjection(ParseState *pstate,
					   char *funcname,
					   Node *first_arg);
static Oid **argtype_inherit(int nargs, Oid *argtypes);

static int	find_inheritors(Oid relid, Oid **supervec);
static CandidateList func_get_candidates(char *funcname, int nargs);
static Oid **gen_cross_product(InhPaths *arginh, int nargs);
static void make_arguments(ParseState *pstate,
			   int nargs,
			   List *fargs,
			   Oid *input_typeids,
			   Oid *function_typeids);
static int match_argtypes(int nargs,
			   Oid *input_typeids,
			   CandidateList function_typeids,
			   CandidateList *candidates);
static FieldSelect *setup_field_select(Node *input, char *attname, Oid relid);
static Oid *func_select_candidate(int nargs, Oid *input_typeids,
					  CandidateList candidates);
static int	agg_get_candidates(char *aggname, Oid typeId, CandidateList *candidates);
static Oid	agg_select_candidate(Oid typeid, CandidateList candidates);


/*
 *	Parse a function call
 *
 *	For historical reasons, Postgres tries to treat the notations tab.col
 *	and col(tab) as equivalent: if a single-argument function call has an
 *	argument of complex type and the function name matches any attribute
 *	of the type, we take it as a column projection.
 *
 *	Hence, both cases come through here.  The is_column parameter tells us
 *	which syntactic construct is actually being dealt with, but this is
 *	intended to be used only to deliver an appropriate error message,
 *	not to affect the semantics.  When is_column is true, we should have
 *	a single argument (the putative table), function name equal to the
 *	column name, and no aggregate decoration.
 *
 *	In the function-call case, the argument expressions have been transformed
 *	already.  In the column case, we may get either a transformed expression
 *	or a RangeVar node as argument.
 */
Node *
ParseFuncOrColumn(ParseState *pstate, char *funcname, List *fargs,
				  bool agg_star, bool agg_distinct, bool is_column)
{
	Oid			rettype;
	Oid			funcid;
	List	   *i;
	Node	   *first_arg = NULL;
	char	   *refname;
	int			nargs = length(fargs);
	int			argn;
	Func	   *funcnode;
	Oid			oid_array[FUNC_MAX_ARGS];
	Oid		   *true_oid_array;
	Node	   *retval;
	bool		retset;
	bool		must_be_agg = agg_star || agg_distinct;
	bool		could_be_agg;
	Expr	   *expr;
	FuncDetailCode fdresult;

	/*
	 * Most of the rest of the parser just assumes that functions do not
	 * have more than FUNC_MAX_ARGS parameters.  We have to test here to
	 * protect against array overruns, etc.  Of course, this may not be a
	 * function, but the test doesn't hurt.
	 */
	if (nargs > FUNC_MAX_ARGS)
		elog(ERROR, "Cannot pass more than %d arguments to a function",
			 FUNC_MAX_ARGS);

	if (fargs)
	{
		first_arg = lfirst(fargs);
		if (first_arg == NULL)	/* should not happen */
			elog(ERROR, "Function '%s' does not allow NULL input", funcname);
	}

	/*
	 * check for column projection: if function has one argument, and that
	 * argument is of complex type, then the function could be a projection.
	 */
	/* We only have one parameter, and it's not got aggregate decoration */
	if (nargs == 1 && !must_be_agg)
	{
		/* Is it a not-yet-transformed RangeVar node? */
		if (IsA(first_arg, RangeVar))
		{
			/* First arg is a relation. This could be a projection. */
			refname = ((RangeVar *) first_arg)->relname;

			retval = qualifiedNameToVar(pstate, refname, funcname, true);
			if (retval)
				return retval;
		}
		else if (ISCOMPLEX(exprType(first_arg)))
		{
			/*
			 * Attempt to handle projection of a complex argument. If
			 * ParseComplexProjection can't handle the projection, we have
			 * to keep going.
			 */
			retval = ParseComplexProjection(pstate,
											funcname,
											first_arg);
			if (retval)
				return retval;
		}
	}

	/*
	 * See if it's an aggregate.
	 */
	if (must_be_agg)
	{
		/* We don't presently cope with, eg, foo(DISTINCT x,y) */
		if (nargs != 1)
			elog(ERROR, "Aggregate functions may only have one parameter");
		/* Agg's argument can't be a relation name, either */
		if (IsA(first_arg, RangeVar))
			elog(ERROR, "Aggregate functions cannot be applied to relation names");
		could_be_agg = true;
	}
	else
	{
		/* Try to parse as an aggregate if above-mentioned checks are OK */
		could_be_agg = (nargs == 1) && !(IsA(first_arg, RangeVar));
	}

	if (could_be_agg)
	{
		Oid			basetype = exprType(lfirst(fargs));
		int			ncandidates;
		CandidateList candidates;

		/* try for exact match first... */
		if (SearchSysCacheExists(AGGNAME,
								 PointerGetDatum(funcname),
								 ObjectIdGetDatum(basetype),
								 0, 0))
			return (Node *) ParseAgg(pstate, funcname, basetype,
									 fargs, agg_star, agg_distinct);

		/* check for aggregate-that-accepts-any-type (eg, COUNT) */
		if (SearchSysCacheExists(AGGNAME,
								 PointerGetDatum(funcname),
								 ObjectIdGetDatum(0),
								 0, 0))
			return (Node *) ParseAgg(pstate, funcname, 0,
									 fargs, agg_star, agg_distinct);

		/*
		 * No exact match yet, so see if there is another entry in the
		 * aggregate table that is compatible. - thomas 1998-12-05
		 */
		ncandidates = agg_get_candidates(funcname, basetype, &candidates);
		if (ncandidates > 0)
		{
			Oid			type;

			type = agg_select_candidate(basetype, candidates);
			if (OidIsValid(type))
			{
				lfirst(fargs) = coerce_type(pstate, lfirst(fargs),
											basetype, type, -1);
				basetype = type;
				return (Node *) ParseAgg(pstate, funcname, basetype,
										 fargs, agg_star, agg_distinct);
			}
			else
			{
				/* Multiple possible matches --- give up */
				elog(ERROR, "Unable to select an aggregate function %s(%s)",
					 funcname, format_type_be(basetype));
			}
		}

		if (must_be_agg)
		{
			/*
			 * No matching agg, but we had '*' or DISTINCT, so a plain
			 * function could not have been meant.
			 */
			elog(ERROR, "There is no aggregate function %s(%s)",
				 funcname, format_type_be(basetype));
		}
	}

	/*
	 * Okay, it's not a column projection, so it must really be a function.
	 * Extract arg type info and transform RangeVar arguments into varnodes
	 * of the appropriate form.
	 */
	MemSet(oid_array, 0, FUNC_MAX_ARGS * sizeof(Oid));

	argn = 0;
	foreach(i, fargs)
	{
		Node	   *arg = lfirst(i);
		Oid			toid;

		if (IsA(arg, RangeVar))
		{
			RangeTblEntry *rte;
			int			vnum;
			int			sublevels_up;

			/*
			 * a relation
			 */
			refname = ((RangeVar *) arg)->relname;

			rte = refnameRangeTblEntry(pstate, refname,
									   &sublevels_up);

			if (rte == NULL)
				rte = addImplicitRTE(pstate, refname);

			vnum = RTERangeTablePosn(pstate, rte, &sublevels_up);

			/*
			 * The parameter to be passed to the function is the whole
			 * tuple from the relation.  We build a special VarNode to
			 * reflect this -- it has varno set to the correct range table
			 * entry, but has varattno == 0 to signal that the whole tuple
			 * is the argument.  Also, it has typmod set to
			 * sizeof(Pointer) to signal that the runtime representation
			 * will be a pointer not an Oid.
			 */
			if (rte->rtekind != RTE_RELATION)
			{
				/*
				 * RTE is a join or subselect; must fail for lack of a
				 * named tuple type
				 */
				if (is_column)
					elog(ERROR, "No such attribute %s.%s",
						 refname, funcname);
				else
				{
					elog(ERROR, "Cannot pass result of sub-select or join %s to a function",
						 refname);
				}
			}

			toid = typenameTypeId(rte->relname);

			/* replace RangeVar in the arg list */
			lfirst(i) = makeVar(vnum,
								InvalidAttrNumber,
								toid,
								sizeof(Pointer),
								sublevels_up);
		}
		else
			toid = exprType(arg);

		oid_array[argn++] = toid;
	}

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance,
	 * and returns the funcid and type and set or singleton status of
	 * the function's return value.  it also returns the true argument
	 * types to the function.
	 */
	fdresult = func_get_detail(funcname, fargs, nargs, oid_array,
							   &funcid, &rettype, &retset,
							   &true_oid_array);
	if (fdresult == FUNCDETAIL_COERCION)
	{
		/*
		 * We can do it as a trivial coercion. coerce_type can handle
		 * these cases, so why duplicate code...
		 */
		return coerce_type(pstate, lfirst(fargs),
						   oid_array[0], rettype, -1);
	}
	if (fdresult != FUNCDETAIL_NORMAL)
	{
		/*
		 * Oops.  Time to die.
		 *
		 * If we are dealing with the attribute notation rel.function,
		 * give an error message that is appropriate for that case.
		 */
		if (is_column)
			elog(ERROR, "Attribute \"%s\" not found", funcname);
		/* Else generate a detailed complaint */
		func_error(NULL, funcname, nargs, oid_array,
				   "Unable to identify a function that satisfies the "
				   "given argument types"
				   "\n\tYou may need to add explicit typecasts");
	}

	/* got it */
	funcnode = makeNode(Func);
	funcnode->funcid = funcid;
	funcnode->functype = rettype;
	funcnode->func_fcache = NULL;

	/* perform the necessary typecasting of arguments */
	make_arguments(pstate, nargs, fargs, oid_array, true_oid_array);

	expr = makeNode(Expr);
	expr->typeOid = rettype;
	expr->opType = FUNC_EXPR;
	expr->oper = (Node *) funcnode;
	expr->args = fargs;
	retval = (Node *) expr;

	/*
	 * if the function returns a set of values, then we need to iterate
	 * over all the returned values in the executor, so we stick an iter
	 * node here.  if it returns a singleton, then we don't need the iter
	 * node.
	 */
	if (retset)
	{
		Iter	   *iter = makeNode(Iter);

		iter->itertype = rettype;
		iter->iterexpr = retval;
		retval = (Node *) iter;
	}

	return retval;
}


static int
agg_get_candidates(char *aggname,
				   Oid typeId,
				   CandidateList *candidates)
{
	Relation	pg_aggregate_desc;
	SysScanDesc	pg_aggregate_scan;
	HeapTuple	tup;
	int			ncandidates = 0;
	ScanKeyData aggKey[1];

	*candidates = NULL;

	ScanKeyEntryInitialize(&aggKey[0], 0,
						   Anum_pg_aggregate_aggname,
						   F_NAMEEQ,
						   NameGetDatum(aggname));

	pg_aggregate_desc = heap_openr(AggregateRelationName, AccessShareLock);
	pg_aggregate_scan = systable_beginscan(pg_aggregate_desc,
										   AggregateNameTypeIndex, true,
										   SnapshotNow,
										   1, aggKey);

	while (HeapTupleIsValid(tup = systable_getnext(pg_aggregate_scan)))
	{
		Form_pg_aggregate agg = (Form_pg_aggregate) GETSTRUCT(tup);
		CandidateList current_candidate;

		current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *) palloc(sizeof(Oid));

		current_candidate->args[0] = agg->aggbasetype;
		current_candidate->next = *candidates;
		*candidates = current_candidate;
		ncandidates++;
	}

	systable_endscan(pg_aggregate_scan);
	heap_close(pg_aggregate_desc, AccessShareLock);

	return ncandidates;
}	/* agg_get_candidates() */

/* agg_select_candidate()
 *
 * Try to choose only one candidate aggregate function from a list of
 * possible matches.  Return value is Oid of input type of aggregate
 * if successful, else InvalidOid.
 */
static Oid
agg_select_candidate(Oid typeid, CandidateList candidates)
{
	CandidateList current_candidate;
	CandidateList last_candidate;
	Oid			current_typeid;
	int			ncandidates;
	CATEGORY	category,
				current_category;

	/*
	 * First look for exact matches or binary compatible matches. (Of
	 * course exact matches shouldn't even get here, but anyway.)
	 */
	ncandidates = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeid = current_candidate->args[0];

		if (IsBinaryCompatible(current_typeid, typeid))
		{
			last_candidate = current_candidate;
			ncandidates++;
		}
	}
	if (ncandidates == 1)
		return last_candidate->args[0];

	/*
	 * If no luck that way, look for candidates which allow coercion and
	 * have a preferred type. Keep all candidates if none match.
	 */
	category = TypeCategory(typeid);
	ncandidates = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeid = current_candidate->args[0];
		current_category = TypeCategory(current_typeid);

		if (current_category == category
			&& IsPreferredType(current_category, current_typeid)
			&& can_coerce_type(1, &typeid, &current_typeid))
		{
			/* only one so far? then keep it... */
			if (last_candidate == NULL)
			{
				candidates = current_candidate;
				last_candidate = current_candidate;
				ncandidates = 1;
			}
			/* otherwise, keep this one too... */
			else
			{
				last_candidate->next = current_candidate;
				last_candidate = current_candidate;
				ncandidates++;
			}
		}
		/* otherwise, don't bother keeping this one around... */
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates->args[0];

	return InvalidOid;
}	/* agg_select_candidate() */


/* func_get_candidates()
 * get a list of all argument type vectors for which a function named
 * funcname taking nargs arguments exists
 */
static CandidateList
func_get_candidates(char *funcname, int nargs)
{
	Relation	heapRelation;
	ScanKeyData skey[2];
	HeapTuple	tuple;
	SysScanDesc	funcscan;
	CandidateList candidates = NULL;
	int			i;

	heapRelation = heap_openr(ProcedureRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_proc_proname,
						   (RegProcedure) F_NAMEEQ,
						   PointerGetDatum(funcname));
	ScanKeyEntryInitialize(&skey[1],
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_proc_pronargs,
						   (RegProcedure) F_INT2EQ,
						   Int16GetDatum(nargs));

	funcscan = systable_beginscan(heapRelation, ProcedureNameIndex, true,
								  SnapshotNow, 2, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(funcscan)))
	{
		Form_pg_proc pgProcP = (Form_pg_proc) GETSTRUCT(tuple);
		CandidateList current_candidate;

		current_candidate = (CandidateList)
			palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *)
			palloc(FUNC_MAX_ARGS * sizeof(Oid));
		MemSet(current_candidate->args, 0, FUNC_MAX_ARGS * sizeof(Oid));
		for (i = 0; i < nargs; i++)
			current_candidate->args[i] = pgProcP->proargtypes[i];

		current_candidate->next = candidates;
		candidates = current_candidate;
	}

	systable_endscan(funcscan);
	heap_close(heapRelation, AccessShareLock);

	return candidates;
}


/* match_argtypes()
 * Given a list of possible typeid arrays to a function and an array of
 * input typeids, produce a shortlist of those function typeid arrays
 * that match the input typeids (either exactly or by coercion), and
 * return the number of such arrays
 */
static int
match_argtypes(int nargs,
			   Oid *input_typeids,
			   CandidateList function_typeids,
			   CandidateList *candidates)		/* return value */
{
	CandidateList current_candidate;
	CandidateList matching_candidate;
	Oid		   *current_typeids;
	int			ncandidates = 0;

	*candidates = NULL;

	for (current_candidate = function_typeids;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		if (can_coerce_type(nargs, input_typeids, current_typeids))
		{
			matching_candidate = (CandidateList)
				palloc(sizeof(struct _CandidateList));
			matching_candidate->args = current_typeids;
			matching_candidate->next = *candidates;
			*candidates = matching_candidate;
			ncandidates++;
		}
	}

	return ncandidates;
}	/* match_argtypes() */


/* func_select_candidate()
 * Given the input argtype array and more than one candidate
 * for the function argtype array, attempt to resolve the conflict.
 * Returns the selected argtype array if the conflict can be resolved,
 * otherwise returns NULL.
 *
 * By design, this is pretty similar to oper_select_candidate in parse_oper.c.
 * However, the calling convention is a little different: we assume the caller
 * already pruned away "candidates" that aren't actually coercion-compatible
 * with the input types, whereas oper_select_candidate must do that itself.
 */
static Oid *
func_select_candidate(int nargs,
					  Oid *input_typeids,
					  CandidateList candidates)
{
	CandidateList current_candidate;
	CandidateList last_candidate;
	Oid		   *current_typeids;
	Oid			current_type;
	int			i;
	int			ncandidates;
	int			nbestMatch,
				nmatch;
	CATEGORY	slot_category[FUNC_MAX_ARGS],
				current_category;
	bool		slot_has_preferred_type[FUNC_MAX_ARGS];
	bool		resolved_unknowns;

	/*
	 * Run through all candidates and keep those with the most matches on
	 * exact types. Keep all candidates if none match.
	 */
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_typeids[i] != UNKNOWNOID &&
				current_typeids[i] == input_typeids[i])
				nmatch++;
		}

		/* take this one as the best choice so far? */
		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		/* no worse than the last choice, so keep this one too? */
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
		/* otherwise, don't bother keeping this one... */
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates->args;

	/*
	 * Still too many candidates? Run through all candidates and keep
	 * those with the most matches on exact types + binary-compatible
	 * types. Keep all candidates if none match.
	 */
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_typeids[i] != UNKNOWNOID)
			{
				if (IsBinaryCompatible(current_typeids[i], input_typeids[i]))
					nmatch++;
			}
		}

		/* take this one as the best choice so far? */
		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		/* no worse than the last choice, so keep this one too? */
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
		/* otherwise, don't bother keeping this one... */
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates->args;

	/*
	 * Still too many candidates? Now look for candidates which are
	 * preferred types at the args that will require coercion. Keep all
	 * candidates if none match.
	 */
	ncandidates = 0;
	nbestMatch = 0;
	last_candidate = NULL;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
	{
		current_typeids = current_candidate->args;
		nmatch = 0;
		for (i = 0; i < nargs; i++)
		{
			if (input_typeids[i] != UNKNOWNOID)
			{
				current_category = TypeCategory(current_typeids[i]);
				if (current_typeids[i] == input_typeids[i] ||
					IsPreferredType(current_category, current_typeids[i]))
					nmatch++;
			}
		}

		if ((nmatch > nbestMatch) || (last_candidate == NULL))
		{
			nbestMatch = nmatch;
			candidates = current_candidate;
			last_candidate = current_candidate;
			ncandidates = 1;
		}
		else if (nmatch == nbestMatch)
		{
			last_candidate->next = current_candidate;
			last_candidate = current_candidate;
			ncandidates++;
		}
	}

	if (last_candidate)			/* terminate rebuilt list */
		last_candidate->next = NULL;

	if (ncandidates == 1)
		return candidates->args;

	/*
	 * Still too many candidates? Try assigning types for the unknown
	 * columns.
	 *
	 * We do this by examining each unknown argument position to see if we
	 * can determine a "type category" for it.	If any candidate has an
	 * input datatype of STRING category, use STRING category (this bias
	 * towards STRING is appropriate since unknown-type literals look like
	 * strings).  Otherwise, if all the candidates agree on the type
	 * category of this argument position, use that category.  Otherwise,
	 * fail because we cannot determine a category.
	 *
	 * If we are able to determine a type category, also notice whether any
	 * of the candidates takes a preferred datatype within the category.
	 *
	 * Having completed this examination, remove candidates that accept the
	 * wrong category at any unknown position.	Also, if at least one
	 * candidate accepted a preferred type at a position, remove
	 * candidates that accept non-preferred types.
	 *
	 * If we are down to one candidate at the end, we win.
	 */
	resolved_unknowns = false;
	for (i = 0; i < nargs; i++)
	{
		bool		have_conflict;

		if (input_typeids[i] != UNKNOWNOID)
			continue;
		resolved_unknowns = true;		/* assume we can do it */
		slot_category[i] = INVALID_TYPE;
		slot_has_preferred_type[i] = false;
		have_conflict = false;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			current_typeids = current_candidate->args;
			current_type = current_typeids[i];
			current_category = TypeCategory(current_type);
			if (slot_category[i] == INVALID_TYPE)
			{
				/* first candidate */
				slot_category[i] = current_category;
				slot_has_preferred_type[i] =
					IsPreferredType(current_category, current_type);
			}
			else if (current_category == slot_category[i])
			{
				/* more candidates in same category */
				slot_has_preferred_type[i] |=
					IsPreferredType(current_category, current_type);
			}
			else
			{
				/* category conflict! */
				if (current_category == STRING_TYPE)
				{
					/* STRING always wins if available */
					slot_category[i] = current_category;
					slot_has_preferred_type[i] =
						IsPreferredType(current_category, current_type);
				}
				else
				{
					/*
					 * Remember conflict, but keep going (might find
					 * STRING)
					 */
					have_conflict = true;
				}
			}
		}
		if (have_conflict && slot_category[i] != STRING_TYPE)
		{
			/* Failed to resolve category conflict at this position */
			resolved_unknowns = false;
			break;
		}
	}

	if (resolved_unknowns)
	{
		/* Strip non-matching candidates */
		ncandidates = 0;
		last_candidate = NULL;
		for (current_candidate = candidates;
			 current_candidate != NULL;
			 current_candidate = current_candidate->next)
		{
			bool		keepit = true;

			current_typeids = current_candidate->args;
			for (i = 0; i < nargs; i++)
			{
				if (input_typeids[i] != UNKNOWNOID)
					continue;
				current_type = current_typeids[i];
				current_category = TypeCategory(current_type);
				if (current_category != slot_category[i])
				{
					keepit = false;
					break;
				}
				if (slot_has_preferred_type[i] &&
					!IsPreferredType(current_category, current_type))
				{
					keepit = false;
					break;
				}
			}
			if (keepit)
			{
				/* keep this candidate */
				last_candidate = current_candidate;
				ncandidates++;
			}
			else
			{
				/* forget this candidate */
				if (last_candidate)
					last_candidate->next = current_candidate->next;
				else
					candidates = current_candidate->next;
			}
		}
		if (last_candidate)		/* terminate rebuilt list */
			last_candidate->next = NULL;
	}

	if (ncandidates == 1)
		return candidates->args;

	return NULL;				/* failed to determine a unique candidate */
}	/* func_select_candidate() */


/* func_get_detail()
 *
 * Find the named function in the system catalogs.
 *
 * Attempt to find the named function in the system catalogs with
 *	arguments exactly as specified, so that the normal case
 *	(exact match) is as quick as possible.
 *
 * If an exact match isn't found:
 *	1) check for possible interpretation as a trivial type coercion
 *	2) get a vector of all possible input arg type arrays constructed
 *	   from the superclasses of the original input arg types
 *	3) get a list of all possible argument type arrays to the function
 *	   with given name and number of arguments
 *	4) for each input arg type array from vector #1:
 *	 a) find how many of the function arg type arrays from list #2
 *		it can be coerced to
 *	 b) if the answer is one, we have our function
 *	 c) if the answer is more than one, attempt to resolve the conflict
 *	 d) if the answer is zero, try the next array from vector #1
 */
FuncDetailCode
func_get_detail(char *funcname,
				List *fargs,
				int nargs,
				Oid *argtypes,
				Oid *funcid,	/* return value */
				Oid *rettype,	/* return value */
				bool *retset,	/* return value */
				Oid **true_typeids)		/* return value */
{
	HeapTuple	ftup;
	CandidateList function_typeids;

	/* attempt to find with arguments exactly as specified... */
	ftup = SearchSysCache(PROCNAME,
						  PointerGetDatum(funcname),
						  Int32GetDatum(nargs),
						  PointerGetDatum(argtypes),
						  0);

	if (HeapTupleIsValid(ftup))
	{
		/* given argument types are the right ones */
		*true_typeids = argtypes;
	}
	else
	{
		/*
		 * If we didn't find an exact match, next consider the possibility
		 * that this is really a type-coercion request: a single-argument
		 * function call where the function name is a type name.  If so,
		 * and if we can do the coercion trivially (no run-time function
		 * call needed), then go ahead and treat the "function call" as a
		 * coercion.  This interpretation needs to be given higher
		 * priority than interpretations involving a type coercion
		 * followed by a function call, otherwise we can produce
		 * surprising results. For example, we want "text(varchar)" to be
		 * interpreted as a trivial coercion, not as "text(name(varchar))"
		 * which the code below this point is entirely capable of
		 * selecting.
		 *
		 * "Trivial" coercions are ones that involve binary-compatible types
		 * and ones that are coercing a previously-unknown-type literal
		 * constant to a specific type.
		 *
		 * NB: it's important that this code stays in sync with what
		 * coerce_type can do, because the caller will try to apply
		 * coerce_type if we return FUNCDETAIL_COERCION.  If we return
		 * that result for something coerce_type can't handle, we'll cause
		 * infinite recursion between this module and coerce_type!
		 */
		if (nargs == 1)
		{
			Oid			targetType;

			targetType = GetSysCacheOid(TYPENAME,
										PointerGetDatum(funcname),
										0, 0, 0);
			if (OidIsValid(targetType) &&
				!ISCOMPLEX(targetType))
			{
				Oid			sourceType = argtypes[0];
				Node	   *arg1 = lfirst(fargs);

				if ((sourceType == UNKNOWNOID && IsA(arg1, Const)) ||
					IsBinaryCompatible(sourceType, targetType))
				{
					/* Yup, it's a type coercion */
					*funcid = InvalidOid;
					*rettype = targetType;
					*retset = false;
					*true_typeids = argtypes;
					return FUNCDETAIL_COERCION;
				}
			}
		}

		/*
		 * didn't find an exact match, so now try to match up
		 * candidates...
		 */

		function_typeids = func_get_candidates(funcname, nargs);

		/* found something, so let's look through them... */
		if (function_typeids != NULL)
		{
			Oid		  **input_typeid_vector = NULL;
			Oid		   *current_input_typeids;

			/*
			 * First we will search with the given argtypes, then with
			 * variants based on replacing complex types with their
			 * inheritance ancestors.  Stop as soon as any match is found.
			 */
			current_input_typeids = argtypes;

			do
			{
				CandidateList current_function_typeids;
				int			ncandidates;

				ncandidates = match_argtypes(nargs, current_input_typeids,
											 function_typeids,
											 &current_function_typeids);

				/* one match only? then run with it... */
				if (ncandidates == 1)
				{
					*true_typeids = current_function_typeids->args;
					ftup = SearchSysCache(PROCNAME,
										  PointerGetDatum(funcname),
										  Int32GetDatum(nargs),
										  PointerGetDatum(*true_typeids),
										  0);
					Assert(HeapTupleIsValid(ftup));
					break;
				}

				/*
				 * multiple candidates? then better decide or throw an
				 * error...
				 */
				if (ncandidates > 1)
				{
					*true_typeids = func_select_candidate(nargs,
												   current_input_typeids,
											   current_function_typeids);

					if (*true_typeids != NULL)
					{
						/* was able to choose a best candidate */
						ftup = SearchSysCache(PROCNAME,
											  PointerGetDatum(funcname),
											  Int32GetDatum(nargs),
										  PointerGetDatum(*true_typeids),
											  0);
						Assert(HeapTupleIsValid(ftup));
						break;
					}

					/*
					 * otherwise, ambiguous function call, so fail by
					 * exiting loop with ftup still NULL.
					 */
					break;
				}

				/*
				 * No match here, so try the next inherited type vector.
				 * First time through, we need to compute the list of
				 * vectors.
				 */
				if (input_typeid_vector == NULL)
					input_typeid_vector = argtype_inherit(nargs, argtypes);

				current_input_typeids = *input_typeid_vector++;
			}
			while (current_input_typeids != NULL);
		}
	}

	if (HeapTupleIsValid(ftup))
	{
		Form_pg_proc pform = (Form_pg_proc) GETSTRUCT(ftup);

		*funcid = ftup->t_data->t_oid;
		*rettype = pform->prorettype;
		*retset = pform->proretset;
		ReleaseSysCache(ftup);
		return FUNCDETAIL_NORMAL;
	}

	return FUNCDETAIL_NOTFOUND;
}	/* func_get_detail() */

/*
 *	argtype_inherit() -- Construct an argtype vector reflecting the
 *						 inheritance properties of the supplied argv.
 *
 *		This function is used to disambiguate among functions with the
 *		same name but different signatures.  It takes an array of input
 *		type ids.  For each type id in the array that's a complex type
 *		(a class), it walks up the inheritance tree, finding all
 *		superclasses of that type.	A vector of new Oid type arrays
 *		is returned to the caller, reflecting the structure of the
 *		inheritance tree above the supplied arguments.
 *
 *		The order of this vector is as follows:  all superclasses of the
 *		rightmost complex class are explored first.  The exploration
 *		continues from right to left.  This policy means that we favor
 *		keeping the leftmost argument type as low in the inheritance tree
 *		as possible.  This is intentional; it is exactly what we need to
 *		do for method dispatch.  The last type array we return is all
 *		zeroes.  This will match any functions for which return types are
 *		not defined.  There are lots of these (mostly builtins) in the
 *		catalogs.
 */
static Oid **
argtype_inherit(int nargs, Oid *argtypes)
{
	Oid			relid;
	int			i;
	InhPaths	arginh[FUNC_MAX_ARGS];

	for (i = 0; i < FUNC_MAX_ARGS; i++)
	{
		if (i < nargs)
		{
			arginh[i].self = argtypes[i];
			if ((relid = typeidTypeRelid(argtypes[i])) != InvalidOid)
				arginh[i].nsupers = find_inheritors(relid, &(arginh[i].supervec));
			else
			{
				arginh[i].nsupers = 0;
				arginh[i].supervec = (Oid *) NULL;
			}
		}
		else
		{
			arginh[i].self = InvalidOid;
			arginh[i].nsupers = 0;
			arginh[i].supervec = (Oid *) NULL;
		}
	}

	/* return an ordered cross-product of the classes involved */
	return gen_cross_product(arginh, nargs);
}

static int
find_inheritors(Oid relid, Oid **supervec)
{
	Relation	inhrel;
	HeapScanDesc inhscan;
	ScanKeyData skey;
	HeapTuple	inhtup;
	Oid		   *relidvec;
	int			nvisited;
	List	   *visited,
			   *queue;
	List	   *elt;
	bool		newrelid;

	nvisited = 0;
	queue = NIL;
	visited = NIL;

	inhrel = heap_openr(InheritsRelationName, AccessShareLock);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph
	 * from the relid supplied up to the root.	At the top of the loop,
	 * relid is the OID of the reltype to check next, queue is the list of
	 * pending rels to check after this one, and visited is the list of
	 * relids we need to output.
	 */
	do
	{
		/* find all types this relid inherits from, and add them to queue */

		ScanKeyEntryInitialize(&skey, 0x0, Anum_pg_inherits_inhrelid,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		inhscan = heap_beginscan(inhrel, 0, SnapshotNow, 1, &skey);

		while (HeapTupleIsValid(inhtup = heap_getnext(inhscan, 0)))
		{
			Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inhtup);

			queue = lappendi(queue, inh->inhparent);
		}

		heap_endscan(inhscan);

		/* pull next unvisited relid off the queue */

		newrelid = false;
		while (queue != NIL)
		{
			relid = lfirsti(queue);
			queue = lnext(queue);
			if (!intMember(relid, visited))
			{
				newrelid = true;
				break;
			}
		}

		if (newrelid)
		{
			visited = lappendi(visited, relid);
			nvisited++;
		}
	} while (newrelid);

	heap_close(inhrel, AccessShareLock);

	if (nvisited > 0)
	{
		relidvec = (Oid *) palloc(nvisited * sizeof(Oid));
		*supervec = relidvec;

		foreach(elt, visited)
		{
			/* return the type id, rather than the relation id */
			Relation	rd;
			Oid			trelid;

			relid = lfirsti(elt);
			rd = heap_open(relid, NoLock);
			trelid = typenameTypeId(RelationGetRelationName(rd));
			heap_close(rd, NoLock);
			*relidvec++ = trelid;
		}
	}
	else
		*supervec = (Oid *) NULL;

	freeList(visited);

	/*
	 * there doesn't seem to be any equally easy way to release the queue
	 * list cells, but since they're palloc'd space it's not critical.
	 */

	return nvisited;
}

static Oid **
gen_cross_product(InhPaths *arginh, int nargs)
{
	int			nanswers;
	Oid		  **result,
			  **iter;
	Oid		   *oneres;
	int			i,
				j;
	int			cur[FUNC_MAX_ARGS];

	nanswers = 1;
	for (i = 0; i < nargs; i++)
	{
		nanswers *= (arginh[i].nsupers + 2);
		cur[i] = 0;
	}

	iter = result = (Oid **) palloc(sizeof(Oid *) * nanswers);

	/* compute the cross product from right to left */
	for (;;)
	{
		oneres = (Oid *) palloc(FUNC_MAX_ARGS * sizeof(Oid));
		MemSet(oneres, 0, FUNC_MAX_ARGS * sizeof(Oid));

		for (i = nargs - 1; i >= 0 && cur[i] > arginh[i].nsupers; i--)
			continue;

		/* if we're done, terminate with NULL pointer */
		if (i < 0)
		{
			*iter = NULL;
			return result;
		}

		/* no, increment this column and zero the ones after it */
		cur[i] = cur[i] + 1;
		for (j = nargs - 1; j > i; j--)
			cur[j] = 0;

		for (i = 0; i < nargs; i++)
		{
			if (cur[i] == 0)
				oneres[i] = arginh[i].self;
			else if (cur[i] > arginh[i].nsupers)
				oneres[i] = 0;	/* wild card */
			else
				oneres[i] = arginh[i].supervec[cur[i] - 1];
		}

		*iter++ = oneres;
	}
}


/*
 * Given two type OIDs, determine whether the first is a complex type
 * (class type) that inherits from the second.
 */
bool
typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId)
{
	Oid			relid;
	Oid		   *supervec;
	int			nsupers,
				i;
	bool		result;

	if (!ISCOMPLEX(subclassTypeId) || !ISCOMPLEX(superclassTypeId))
		return false;
	relid = typeidTypeRelid(subclassTypeId);
	if (relid == InvalidOid)
		return false;
	nsupers = find_inheritors(relid, &supervec);
	result = false;
	for (i = 0; i < nsupers; i++)
	{
		if (supervec[i] == superclassTypeId)
		{
			result = true;
			break;
		}
	}
	if (supervec)
		pfree(supervec);
	return result;
}


/* make_arguments()
 * Given the number and types of arguments to a function, and the
 *	actual arguments and argument types, do the necessary typecasting.
 */
static void
make_arguments(ParseState *pstate,
			   int nargs,
			   List *fargs,
			   Oid *input_typeids,
			   Oid *function_typeids)
{
	List	   *current_fargs;
	int			i;

	for (i = 0, current_fargs = fargs;
		 i < nargs;
		 i++, current_fargs = lnext(current_fargs))
	{
		/* types don't match? then force coercion using a function call... */
		if (input_typeids[i] != function_typeids[i])
		{
			lfirst(current_fargs) = coerce_type(pstate,
												lfirst(current_fargs),
												input_typeids[i],
												function_typeids[i], -1);
		}
	}
}

/*
 * setup_field_select
 *		Build a FieldSelect node that says which attribute to project to.
 *		This routine is called by ParseFuncOrColumn() when we have found
 *		a projection on a function result or parameter.
 */
static FieldSelect *
setup_field_select(Node *input, char *attname, Oid relid)
{
	FieldSelect *fselect = makeNode(FieldSelect);
	AttrNumber	attno;

	attno = get_attnum(relid, attname);

	fselect->arg = input;
	fselect->fieldnum = attno;
	fselect->resulttype = get_atttype(relid, attno);
	fselect->resulttypmod = get_atttypmod(relid, attno);

	return fselect;
}

/*
 * ParseComplexProjection -
 *	  handles function calls with a single argument that is of complex type.
 *	  If the function call is actually a column projection, return a suitably
 *	  transformed expression tree.  If not, return NULL.
 *
 * NB: argument is expected to be transformed already, ie, not a RangeVar.
 */
static Node *
ParseComplexProjection(ParseState *pstate,
					   char *funcname,
					   Node *first_arg)
{
	Oid			argtype = exprType(first_arg);
	Oid			argrelid;
	AttrNumber	attnum;
	FieldSelect *fselect;

	argrelid = typeidTypeRelid(argtype);
	if (!argrelid)
		return NULL;			/* probably should not happen */
	attnum = get_attnum(argrelid, funcname);
	if (attnum == InvalidAttrNumber)
		return NULL;			/* funcname does not match any column */

	/*
	 * Check for special cases where we don't want to return a FieldSelect.
	 */
	switch (nodeTag(first_arg))
	{
		case T_Iter:
			{
				Iter	   *iter = (Iter *) first_arg;

				/*
				 * If it's an Iter, we stick the FieldSelect
				 * *inside* the Iter --- this is klugy, but necessary
				 * because ExecTargetList() currently does the right thing
				 * only when the Iter node is at the top level of a
				 * targetlist item.
				 *
				 * XXX Iter should go away altogether...
				 */
				fselect = setup_field_select(iter->iterexpr,
											 funcname, argrelid);
				iter->iterexpr = (Node *) fselect;
				iter->itertype = fselect->resulttype;
				return (Node *) iter;
				break;
			}
		case T_Var:
			{
				Var		   *var = (Var *) first_arg;

				/*
				 * If the Var is a whole-row tuple, we can just replace it
				 * with a simple Var reference.
				 */
				if (var->varattno == InvalidAttrNumber)
				{
					Oid		vartype;
					int32	vartypmod;

					get_atttypetypmod(argrelid, attnum,
									  &vartype, &vartypmod);

					return (Node *) makeVar(var->varno,
											attnum,
											vartype,
											vartypmod,
											var->varlevelsup);
				}
				break;
			}
		default:
			break;
	}

	/* Else generate a FieldSelect expression */
	fselect = setup_field_select(first_arg, funcname, argrelid);
	return (Node *) fselect;
}

/*
 * Error message when function lookup fails that gives details of the
 * argument types
 */
void
func_error(char *caller, char *funcname, int nargs, Oid *argtypes, char *msg)
{
	char		p[(NAMEDATALEN + 2) * FUNC_MAX_ARGS],
			   *ptr;
	int			i;

	ptr = p;
	*ptr = '\0';
	for (i = 0; i < nargs; i++)
	{
		if (i)
		{
			*ptr++ = ',';
			*ptr++ = ' ';
		}
		if (argtypes[i] != 0)
		{
			strcpy(ptr, typeidTypeName(argtypes[i]));
			*(ptr + NAMEDATALEN) = '\0';
		}
		else
			strcpy(ptr, "opaque");
		ptr += strlen(ptr);
	}

	if (caller == NULL)
	{
		elog(ERROR, "Function '%s(%s)' does not exist%s%s",
			 funcname, p,
			 ((msg != NULL) ? "\n\t" : ""), ((msg != NULL) ? msg : ""));
	}
	else
	{
		elog(ERROR, "%s: function '%s(%s)' does not exist%s%s",
			 caller, funcname, p,
			 ((msg != NULL) ? "\n\t" : ""), ((msg != NULL) ? msg : ""));
	}
}
