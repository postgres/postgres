/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_func.c,v 1.39 1999/02/23 07:51:53 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_aggregate.h"
#include "fmgr.h"
#include "lib/dllist.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/relation.h"
#include "parser/parse_agg.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parse_coerce.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static Node *ParseComplexProjection(ParseState *pstate,
					   char *funcname,
					   Node *first_arg,
					   bool *attisset);
static Oid **argtype_inherit(int nargs, Oid *oid_array);

static int	find_inheritors(Oid relid, Oid **supervec);
static CandidateList func_get_candidates(char *funcname, int nargs);
static bool
func_get_detail(char *funcname,
				int nargs,
				Oid *oid_array,
				Oid *funcid,	/* return value */
				Oid *rettype,	/* return value */
				bool *retset,	/* return value */
				Oid **true_typeids);
static Oid	funcid_get_rettype(Oid funcid);
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
static List *setup_tlist(char *attname, Oid relid);
static List *setup_base_tlist(Oid typeid);
static Oid *func_select_candidate(int nargs, Oid *input_typeids,
				CandidateList candidates);
static int agg_get_candidates(char *aggname, Oid typeId, CandidateList *candidates);
static Oid agg_select_candidate(Oid typeid, CandidateList candidates);

#define ISCOMPLEX(type) (typeidTypeRelid(type) ? true : false)

#define MAXFARGS 8				/* max # args to a c or postquel function */

typedef struct _SuperQE
{
	Oid			sqe_relid;
} SuperQE;

/*
 ** ParseNestedFuncOrColumn 
 **    Given a nested dot expression (i.e. (relation func ... attr), build up
 ** a tree with of Iter and Func nodes.
 */
Node *
ParseNestedFuncOrColumn(ParseState *pstate, Attr *attr, int *curr_resno, int precedence)
{
	List	   *mutator_iter;
	Node	   *retval = NULL;

	if (attr->paramNo != NULL)
	{
		Param	   *param = (Param *) transformExpr(pstate, (Node *) attr->paramNo, EXPR_RELATION_FIRST);

		retval = ParseFuncOrColumn(pstate, strVal(lfirst(attr->attrs)),
								   lcons(param, NIL),
								   curr_resno,
								   precedence);
	}
	else
	{
		Ident	   *ident = makeNode(Ident);

		ident->name = attr->relname;
		ident->isRel = TRUE;
		retval = ParseFuncOrColumn(pstate, strVal(lfirst(attr->attrs)),
								   lcons(ident, NIL),
								   curr_resno,
								   precedence);
	}

	/* Do more attributes follow this one? */
	foreach(mutator_iter, lnext(attr->attrs))
	{
		retval = ParseFuncOrColumn(pstate, strVal(lfirst(mutator_iter)),
								   lcons(retval, NIL),
								   curr_resno,
								   precedence);
	}

	return retval;
}

static int
agg_get_candidates(char *aggname,
				   Oid typeId,
				   CandidateList *candidates)
{
	CandidateList		current_candidate;
	Relation			pg_aggregate_desc;
	HeapScanDesc		pg_aggregate_scan;
	HeapTuple			tup;
	Form_pg_aggregate	agg;
	int					ncandidates = 0;

	static ScanKeyData aggKey[1] = {
	{0, Anum_pg_aggregate_aggname, F_NAMEEQ}};

	*candidates = NULL;

	fmgr_info(F_NAMEEQ, (FmgrInfo *) &aggKey[0].sk_func);
	aggKey[0].sk_argument = NameGetDatum(aggname);

	pg_aggregate_desc = heap_openr(AggregateRelationName);
	pg_aggregate_scan = heap_beginscan(pg_aggregate_desc,
									   0,
									   SnapshotSelf,     /* ??? */
									   1,
									   aggKey);

	while (HeapTupleIsValid(tup = heap_getnext(pg_aggregate_scan, 0)))
	{
		current_candidate = (CandidateList) palloc(sizeof(struct _CandidateList));
		current_candidate->args = (Oid *) palloc(sizeof(Oid));

		agg = (Form_pg_aggregate) GETSTRUCT(tup);
		current_candidate->args[0] = agg->aggbasetype;
		current_candidate->next = *candidates;
		*candidates = current_candidate;
		ncandidates++;
	}

	heap_endscan(pg_aggregate_scan);
	heap_close(pg_aggregate_desc);

	return ncandidates;
}	/* agg_get_candidates() */

/* agg_select_candidate()
 * Try to choose only one candidate aggregate function from a list of possibles.
 */
static Oid
agg_select_candidate(Oid typeid, CandidateList candidates)
{
	CandidateList	current_candidate;
	CandidateList	last_candidate;
	Oid				current_typeid;
	int				ncandidates;
	CATEGORY		category,
					current_category;

/*
 * Look for candidates which allow coersion and have a preferred type.
 * Keep all candidates if none match.
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

		if ((current_category == category)
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
		else if (last_candidate != NULL)
		{
			last_candidate->next = NULL;
		}
	}

	return ((ncandidates == 1) ? candidates->args[0] : 0);
}   /* agg_select_candidate() */


/*
 * parse function
 */
Node *
ParseFuncOrColumn(ParseState *pstate, char *funcname, List *fargs,
				  int *curr_resno, int precedence)
{
	Oid			rettype = (Oid) 0;
	Oid			argrelid = (Oid) 0;
	Oid			funcid = (Oid) 0;
	List	   *i = NIL;
	Node	   *first_arg = NULL;
	char	   *relname = NULL;
	char	   *refname = NULL;
	Relation	rd;
	Oid			relid;
	int			nargs;
	Func	   *funcnode;
	Oid			oid_array[8];
	Oid		   *true_oid_array;
	Node	   *retval;
	bool		retset;
	bool		attisset = false;
	Oid			toid = (Oid) 0;
	Expr	   *expr;

	if (fargs)
	{
		first_arg = lfirst(fargs);
		if (first_arg == NULL)
			elog(ERROR, "Function '%s' does not allow NULL input", funcname);
	}

	/*
	 * check for projection methods: if function takes one argument, and
	 * that argument is a relation, param, or PQ function returning a
	 * complex * type, then the function could be a projection.
	 */
	/* We only have one parameter */
	if (length(fargs) == 1)
	{
		/* Is is a plain Relation name from the parser? */
		if (nodeTag(first_arg) == T_Ident && ((Ident *) first_arg)->isRel)
		{
			RangeTblEntry *rte;
			Ident	   *ident = (Ident *) first_arg;

			/*
			 * first arg is a relation. This could be a projection.
			 */
			refname = ident->name;

			rte = refnameRangeTableEntry(pstate, refname);
			if (rte == NULL)
				rte = addRangeTableEntry(pstate, refname, refname, FALSE, FALSE);

			relname = rte->relname;
			relid = rte->relid;

			/*
			 * If the attr isn't a set, just make a var for it.  If it is
			 * a set, treat it like a function and drop through.
			 */
			if (get_attnum(relid, funcname) != InvalidAttrNumber)
			{
				return (Node *) make_var(pstate,
										 relid,
										 refname,
										 funcname);
			}
			else
			{
				/* drop through - attr is a set */
				;
			}
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
											first_arg,
											&attisset);
			if (attisset)
			{
				toid = exprType(first_arg);
				rd = heap_openr(typeidTypeName(toid));
				if (RelationIsValid(rd))
				{
					relname = RelationGetRelationName(rd)->data;
					heap_close(rd);
				}
				else
					elog(ERROR, "Type '%s' is not a relation type",
						 typeidTypeName(toid));
				argrelid = typeidTypeRelid(toid);

				/*
				 * A projection contains either an attribute name or the
				 * "*".
				 */
				if ((get_attnum(argrelid, funcname) == InvalidAttrNumber)
					&& strcmp(funcname, "*"))
					elog(ERROR, "Functions on sets are not yet supported");
			}

			if (retval)
				return retval;
		}
		else
		{

			/*
			 * Parsing aggregates.
			 */
			Type			tp;
			Oid				basetype;
			int				ncandidates;
			CandidateList	candidates;


			/*
			 * the aggregate COUNT is a special case, ignore its base
			 * type.  Treat it as zero
			 */
			if (strcmp(funcname, "count") == 0)
				basetype = 0;
			else
				basetype = exprType(lfirst(fargs));

			/* try for exact match first... */
			if (SearchSysCacheTuple(AGGNAME,
									PointerGetDatum(funcname),
									ObjectIdGetDatum(basetype),
									0, 0))
				return (Node *) ParseAgg(pstate, funcname, basetype,
										 fargs, precedence);

			/*
			 * No exact match yet, so see if there is another entry
			 * in the aggregate table which is compatible.
			 * - thomas 1998-12-05
			 */
			ncandidates = agg_get_candidates(funcname, basetype, &candidates);
			if (ncandidates > 0)
			{
				Oid		type;

				type = agg_select_candidate(basetype, candidates);
				if (OidIsValid(type))
				{
					lfirst(fargs) = coerce_type(pstate, lfirst(fargs), basetype, type);
					basetype = type;

					return (Node *) ParseAgg(pstate, funcname, basetype,
											 fargs, precedence);
				}
				else
				{
					elog(ERROR,"Unable to select an aggregate function %s(%s)",
						 funcname, typeidTypeName(basetype));
				}
			}

			/*
			 * See if this is a single argument function with the function
			 * name also a type name and the input argument and type name
			 * binary compatible...
			 * This means that you are trying for a type conversion which does not
			 * need to take place, so we'll just pass through the argument itself.
			 * (make this clearer with some extra brackets - thomas 1998-12-05)
			 */
			if ((HeapTupleIsValid(tp = SearchSysCacheTuple(TYPNAME,
														   PointerGetDatum(funcname),
														   0, 0, 0)))
				&& IS_BINARY_COMPATIBLE(typeTypeId(tp), basetype))
			{
				return ((Node *) lfirst(fargs));
			}
		}
	}


	/*
	 * If we dropped through to here it's really a function (or a set,
	 * which is implemented as a function). Extract arg type info and
	 * transform relation name arguments into varnodes of the appropriate
	 * form.
	 */
	MemSet(&oid_array[0], 0, 8 * sizeof(Oid));

	nargs = 0;
	foreach(i, fargs)
	{
		int			vnum;
		RangeTblEntry *rte;
		Node	   *pair = lfirst(i);

		if (nodeTag(pair) == T_Ident && ((Ident *) pair)->isRel)
		{

			/*
			 * a relation
			 */
			refname = ((Ident *) pair)->name;

			rte = refnameRangeTableEntry(pstate, refname);
			if (rte == NULL)
				rte = addRangeTableEntry(pstate, refname, refname,
										 FALSE, FALSE);
			relname = rte->relname;

			vnum = refnameRangeTablePosn(pstate, rte->refname, NULL);

			/*
			 * for func(relname), the param to the function is the tuple
			 * under consideration.  we build a special VarNode to reflect
			 * this -- it has varno set to the correct range table entry,
			 * but has varattno == 0 to signal that the whole tuple is the
			 * argument.
			 */
			toid = typeTypeId(typenameType(relname));
			/* replace it in the arg list */
			lfirst(fargs) = makeVar(vnum, 0, toid, -1, 0, vnum, 0);
		}
		else if (!attisset)
		{						/* set functions don't have parameters */

			/*
			 * any functiona args which are typed "unknown", but aren't
			 * constants, we don't know what to do with, because we can't
			 * cast them	- jolly
			 */
			if (exprType(pair) == UNKNOWNOID && !IsA(pair, Const))
				elog(ERROR, "There is no function '%s'"
					 " with argument #%d of type UNKNOWN",
					 funcname, nargs);
			else
				toid = exprType(pair);
		}

		oid_array[nargs++] = toid;
	}

	/*
	 * func_get_detail looks up the function in the catalogs, does
	 * disambiguation for polymorphic functions, handles inheritance, and
	 * returns the funcid and type and set or singleton status of the
	 * function's return value.  it also returns the true argument types
	 * to the function.  if func_get_detail returns true, the function
	 * exists.	otherwise, there was an error.
	 */
	if (attisset)
	{							/* we know all of these fields already */

		/*
		 * We create a funcnode with a placeholder function SetEval.
		 * SetEval() never actually gets executed.	When the function
		 * evaluation routines see it, they use the funcid projected out
		 * from the relation as the actual function to call. Example:
		 * retrieve (emp.mgr.name) The plan for this will scan the emp
		 * relation, projecting out the mgr attribute, which is a funcid.
		 * This function is then called (instead of SetEval) and "name" is
		 * projected from its result.
		 */
		funcid = F_SETEVAL;
		rettype = toid;
		retset = true;
		true_oid_array = oid_array;
	}
	else
	{
		bool		exists;

		exists = func_get_detail(funcname, nargs, oid_array, &funcid,
								 &rettype, &retset, &true_oid_array);
		if (!exists)
			elog(ERROR, "No such function '%s' with the specified attributes",
				 funcname);

	}

	/* got it */
	funcnode = makeNode(Func);
	funcnode->funcid = funcid;
	funcnode->functype = rettype;
	funcnode->funcisindex = false;
	funcnode->funcsize = 0;
	funcnode->func_fcache = NULL;
	funcnode->func_tlist = NIL;
	funcnode->func_planlist = NIL;

	/* perform the necessary typecasting */
	make_arguments(pstate, nargs, fargs, oid_array, true_oid_array);

	/*
	 * for functions returning base types, we want to project out the
	 * return value.  set up a target list to do that.	the executor will
	 * ignore these for c functions, and do the right thing for postquel
	 * functions.
	 */

	if (typeidTypeRelid(rettype) == InvalidOid)
		funcnode->func_tlist = setup_base_tlist(rettype);

	/*
	 * For sets, we want to make a targetlist to project out this
	 * attribute of the set tuples.
	 */
	if (attisset)
	{
		if (!strcmp(funcname, "*"))
		{
			funcnode->func_tlist = expandAll(pstate, relname, refname, curr_resno);
		}
		else
		{
			funcnode->func_tlist = setup_tlist(funcname, argrelid);
			rettype = get_atttype(argrelid, get_attnum(argrelid, funcname));
		}
	}

	/*
	 * Sequence handling.
	 */
	if (funcid == F_NEXTVAL ||
		funcid == F_CURRVAL ||
		funcid == F_SETVAL)
	{
		Const	   *seq;
		char	   *seqrel;
		text	   *seqname;
		int32		aclcheck_result = -1;
		extern text *lower(text *string);

		Assert(length(fargs) == ((funcid == F_SETVAL) ? 2 : 1));
		seq = (Const *) lfirst(fargs);
		if (!IsA((Node *) seq, Const))
			elog(ERROR, "Only constant sequence names are acceptable for function '%s'", funcname);
		seqname = lower((text *) DatumGetPointer(seq->constvalue));
		pfree(DatumGetPointer(seq->constvalue));
		seq->constvalue = PointerGetDatum(seqname);
		seqrel = textout(seqname);

		if ((aclcheck_result = pg_aclcheck(seqrel, GetPgUserName(),
					   (((funcid == F_NEXTVAL) || (funcid == F_SETVAL)) ?
						ACL_WR : ACL_RD)))
			!= ACLCHECK_OK)
			elog(ERROR, "%s.%s: %s",
			  seqrel, funcname, aclcheck_error_strings[aclcheck_result]);

		pfree(seqrel);

		if (funcid == F_NEXTVAL && pstate->p_in_where_clause)
			elog(ERROR, "Sequence function nextval is not allowed in WHERE clauses");
		if (funcid == F_SETVAL && pstate->p_in_where_clause)
			elog(ERROR, "Sequence function setval is not allowed in WHERE clauses");
	}

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

static Oid
funcid_get_rettype(Oid funcid)
{
	HeapTuple	func_tuple = NULL;
	Oid			funcrettype = (Oid) 0;

	func_tuple = SearchSysCacheTuple(PROOID,
									 ObjectIdGetDatum(funcid),
									 0, 0, 0);

	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "Function OID %d does not exist", funcid);

	funcrettype = (Oid)
		((Form_pg_proc) GETSTRUCT(func_tuple))->prorettype;

	return funcrettype;
}


/* func_get_candidates()
 * get a list of all argument type vectors for which a function named
 * funcname taking nargs arguments exists
 */
static CandidateList
func_get_candidates(char *funcname, int nargs)
{
	Relation	heapRelation;
	Relation	idesc;
	ScanKeyData skey;
	HeapTupleData	tuple;
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	Form_pg_proc pgProcP;
	CandidateList candidates = NULL;
	CandidateList current_candidate;
	int			i;

	heapRelation = heap_openr(ProcedureRelationName);
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_NAMEEQ,
						   (Datum) funcname);

	idesc = index_openr(ProcedureNameIndex);

	sd = index_beginscan(idesc, false, 1, &skey);

	do
	{
		indexRes = index_getnext(sd, ForwardScanDirection);
		if (indexRes)
		{
			Buffer		buffer;

			tuple.t_self = indexRes->heap_iptr;
			heap_fetch(heapRelation, SnapshotNow, &tuple, &buffer);
			pfree(indexRes);
			if (tuple.t_data != NULL)
			{
				pgProcP = (Form_pg_proc) GETSTRUCT(&tuple);
				if (pgProcP->pronargs == nargs)
				{
					current_candidate = (CandidateList)
						palloc(sizeof(struct _CandidateList));
					current_candidate->args = (Oid *)
						palloc(8 * sizeof(Oid));
					MemSet(current_candidate->args, 0, 8 * sizeof(Oid));
					for (i = 0; i < nargs; i++)
						current_candidate->args[i] = pgProcP->proargtypes[i];

					current_candidate->next = candidates;
					candidates = current_candidate;
				}
				ReleaseBuffer(buffer);
			}
		}
	} while (indexRes);

	index_endscan(sd);
	index_close(idesc);
	heap_close(heapRelation);

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
 * returns the selected argtype array if the conflict can be resolved,
 * otherwise returns NULL.
 *
 * If all input Oids are UNKNOWNOID, then try matching with TEXTOID.
 * Otherwise, could return first function arguments on list of candidates.
 * But for now, return NULL and make the user give a better hint.
 * - thomas 1998-03-17
 */
static Oid *
func_select_candidate(int nargs,
					  Oid *input_typeids,
					  CandidateList candidates)
{
	CandidateList current_candidate;
	CandidateList last_candidate;
	Oid		   *current_typeids;
	int			i;

	int			ncandidates;
	int			nbestMatch,
				nmatch,
				nident;

	CATEGORY	slot_category,
				current_category;
	Oid			slot_type,
				current_type;

/*
 * Run through all candidates and keep those with the most matches
 *	on explicit types. Keep all candidates if none match.
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
		nident = 0;
		for (i = 0; i < nargs; i++)
		{
			if ((input_typeids[i] != UNKNOWNOID)
				&& (current_typeids[i] == input_typeids[i]))
				nmatch++;
			else if (IS_BINARY_COMPATIBLE(current_typeids[i], input_typeids[i]))
				nident++;
		}

		if ((nmatch + nident) == nargs)
			return current_candidate->args;

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
		else
		{
			last_candidate->next = NULL;
		}
	}

	if (ncandidates == 1)
		return candidates->args;

/*
 * Still too many candidates?
 * Try assigning types for the unknown columns.
 */
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] == UNKNOWNOID)
		{
			slot_category = INVALID_TYPE;
			slot_type = InvalidOid;
			for (current_candidate = candidates;
				 current_candidate != NULL;
				 current_candidate = current_candidate->next)
			{
				current_typeids = current_candidate->args;
				current_type = current_typeids[i];
				current_category = TypeCategory(current_typeids[i]);

				if (slot_category == InvalidOid)
				{
					slot_category = current_category;
					slot_type = current_type;
				}
				else if ((current_category != slot_category)
						 && IS_BUILTIN_TYPE(current_type))
				{
					return NULL;
				}
				else if (current_type != slot_type)
				{
					if (IsPreferredType(slot_category, current_type))
					{
						slot_type = current_type;
						candidates = current_candidate;
					}
					else if (IsPreferredType(slot_category, slot_type))
					{
						 candidates->next = current_candidate->next;
					}
				}
			}

			if (slot_type != InvalidOid)
			{
				input_typeids[i] = slot_type;
			}
		}
		else
		{
		}
	}

	ncandidates = 0;
	for (current_candidate = candidates;
		 current_candidate != NULL;
		 current_candidate = current_candidate->next)
		ncandidates++;

	if (ncandidates == 1)
		return candidates->args;

	return NULL;
}	/* func_select_candidate() */


/* func_get_detail()
 * Find the named function in the system catalogs.
 *
 * Attempt to find the named function in the system catalogs with
 *	arguments exactly as specified, so that the normal case
 *	(exact match) is as quick as possible.
 *
 * If an exact match isn't found:
 *	1) get a vector of all possible input arg type arrays constructed
 *	   from the superclasses of the original input arg types
 *	2) get a list of all possible argument type arrays to the function
 *	   with given name and number of arguments
 *	3) for each input arg type array from vector #1:
 *	 a) find how many of the function arg type arrays from list #2
 *		it can be coerced to
 *	 b) if the answer is one, we have our function
 *	 c) if the answer is more than one, attempt to resolve the conflict
 *	 d) if the answer is zero, try the next array from vector #1
 */
static bool
func_get_detail(char *funcname,
				int nargs,
				Oid *oid_array,
				Oid *funcid,	/* return value */
				Oid *rettype,	/* return value */
				bool *retset,	/* return value */
				Oid **true_typeids)		/* return value */
{
	Oid		  **input_typeid_vector;
	Oid		   *current_input_typeids;
	CandidateList function_typeids;
	CandidateList current_function_typeids;
	HeapTuple	ftup;
	Form_pg_proc pform;

	/* attempt to find with arguments exactly as specified... */
	ftup = SearchSysCacheTuple(PRONAME,
							   PointerGetDatum(funcname),
							   Int32GetDatum(nargs),
							   PointerGetDatum(oid_array),
							   0);
	*true_typeids = oid_array;

	/* didn't find an exact match, so now try to match up candidates... */
	if (!HeapTupleIsValid(ftup))
	{
		function_typeids = func_get_candidates(funcname, nargs);

		/* found something, so let's look through them... */
		if (function_typeids != NULL)
		{
			int			ncandidates;

			input_typeid_vector = argtype_inherit(nargs, oid_array);
			current_input_typeids = oid_array;

			do
			{
				ncandidates = match_argtypes(nargs, current_input_typeids,
											 function_typeids,
											 &current_function_typeids);

				/* one match only? then run with it... */
				if (ncandidates == 1)
				{
					*true_typeids = current_function_typeids->args;
					ftup = SearchSysCacheTuple(PRONAME,
											   PointerGetDatum(funcname),
											   Int32GetDatum(nargs),
											   PointerGetDatum(*true_typeids),
											   0);
					Assert(HeapTupleIsValid(ftup));
				}

				/*
				 * multiple candidates? then better decide or throw an
				 * error...
				 */
				else if (ncandidates > 1)
				{
					*true_typeids = func_select_candidate(nargs,
														  current_input_typeids,
														  current_function_typeids);

					/* couldn't decide, so quit */
					if (*true_typeids == NULL)
					{
						func_error(NULL, funcname, nargs, oid_array,
								   "Unable to identify a function which satisfies the given argument types"
								   "\n\tYou will have to retype your query using explicit typecasts");
					}

					/* found something, so use the first one... */
					else
					{
						ftup = SearchSysCacheTuple(PRONAME,
												   PointerGetDatum(funcname),
												   Int32GetDatum(nargs),
												   PointerGetDatum(*true_typeids),
												   0);
						Assert(HeapTupleIsValid(ftup));
					}
				}
				current_input_typeids = *input_typeid_vector++;
			}
			while (current_input_typeids != InvalidOid && ncandidates == 0);
		}
	}

	if (!HeapTupleIsValid(ftup))
	{
		Type		tp;

		if (nargs == 1)
		{
			tp = typeidType(oid_array[0]);
			if (typeTypeFlag(tp) == 'c')
				elog(ERROR, "No such attribute or function '%s'", funcname);
		}
	}
	else
	{
		pform = (Form_pg_proc) GETSTRUCT(ftup);
		*funcid = ftup->t_data->t_oid;
		*rettype = pform->prorettype;
		*retset = pform->proretset;

		return true;
	}

	/* shouldn't reach here */
	return false;
}	/* func_get_detail() */

/*
 *	argtype_inherit() -- Construct an argtype vector reflecting the
 *						 inheritance properties of the supplied argv.
 *
 *		This function is used to disambiguate among functions with the
 *		same name but different signatures.  It takes an array of eight
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
argtype_inherit(int nargs, Oid *oid_array)
{
	Oid			relid;
	int			i;
	InhPaths	arginh[MAXFARGS];

	for (i = 0; i < MAXFARGS; i++)
	{
		if (i < nargs)
		{
			arginh[i].self = oid_array[i];
			if ((relid = typeidTypeRelid(oid_array[i])) != InvalidOid)
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
	Oid		   *relidvec;
	Relation	inhrel;
	HeapScanDesc inhscan;
	ScanKeyData skey;
	HeapTuple	inhtup;
	TupleDesc	inhtupdesc;
	int			nvisited;
	SuperQE    *qentry,
			   *vnode;
	Dllist	   *visited,
			   *queue;
	Dlelem	   *qe,
			   *elt;

	Relation	rd;
	Datum		d;
	bool		newrelid;
	char		isNull;

	nvisited = 0;
	queue = DLNewList();
	visited = DLNewList();


	inhrel = heap_openr(InheritsRelationName);
	inhtupdesc = RelationGetDescr(inhrel);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph
	 * from the relid supplied up to the root.
	 */
	do
	{
		ScanKeyEntryInitialize(&skey, 0x0, Anum_pg_inherits_inhrel,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		inhscan = heap_beginscan(inhrel, 0, SnapshotNow, 1, &skey);

		while (HeapTupleIsValid(inhtup = heap_getnext(inhscan, 0)))
		{
			qentry = (SuperQE *) palloc(sizeof(SuperQE));

			d = fastgetattr(inhtup, Anum_pg_inherits_inhparent,
							inhtupdesc, &isNull);
			qentry->sqe_relid = DatumGetObjectId(d);

			/* put this one on the queue */
			DLAddTail(queue, DLNewElem(qentry));
		}

		heap_endscan(inhscan);

		/* pull next unvisited relid off the queue */
		do
		{
			qe = DLRemHead(queue);
			qentry = qe ? (SuperQE *) DLE_VAL(qe) : NULL;

			if (qentry == (SuperQE *) NULL)
				break;

			relid = qentry->sqe_relid;
			newrelid = true;

			for (elt = DLGetHead(visited); elt; elt = DLGetSucc(elt))
			{
				vnode = (SuperQE *) DLE_VAL(elt);
				if (vnode && (qentry->sqe_relid == vnode->sqe_relid))
				{
					newrelid = false;
					break;
				}
			}
		} while (!newrelid);

		if (qentry != (SuperQE *) NULL)
		{

			/* save the type id, rather than the relation id */
			if ((rd = heap_open(qentry->sqe_relid)) == (Relation) NULL)
				elog(ERROR, "Relid %d does not exist", qentry->sqe_relid);
			qentry->sqe_relid = typeTypeId(typenameType(RelationGetRelationName(rd)->data));
			heap_close(rd);

			DLAddTail(visited, qe);

			nvisited++;
		}
	} while (qentry != (SuperQE *) NULL);

	heap_close(inhrel);

	if (nvisited > 0)
	{
		relidvec = (Oid *) palloc(nvisited * sizeof(Oid));
		*supervec = relidvec;

		for (elt = DLGetHead(visited); elt; elt = DLGetSucc(elt))
		{
			vnode = (SuperQE *) DLE_VAL(elt);
			*relidvec++ = vnode->sqe_relid;
		}

	}
	else
		*supervec = (Oid *) NULL;

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
	int			cur[MAXFARGS];

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
		oneres = (Oid *) palloc(MAXFARGS * sizeof(Oid));
		MemSet(oneres, 0, MAXFARGS * sizeof(Oid));

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


/* make_arguments()
 * Given the number and types of arguments to a function, and the
 *	actual arguments and argument types, do the necessary typecasting.
 *
 * There are two ways an input typeid can differ from a function typeid:
 *	1) the input type inherits the function type, so no typecasting required
 *	2) the input type can be typecast into the function type
 * Right now, we only typecast unknowns, and that is all we check for.
 *
 * func_get_detail() now can find coersions for function arguments which
 *	will make this function executable. So, we need to recover these
 *	results here too.
 * - thomas 1998-03-25
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

		/*
		 * unspecified type for string constant? then use heuristics for
		 * conversion...
		 */
		if (input_typeids[i] == UNKNOWNOID && function_typeids[i] != InvalidOid)
		{
			lfirst(current_fargs) = parser_typecast2(lfirst(current_fargs),
								 input_typeids[i],
								 typeidType(function_typeids[i]),
								 -1);
		}

		/* types don't match? then force coersion using a function call... */
		else if (input_typeids[i] != function_typeids[i])
		{
			lfirst(current_fargs) = coerce_type(pstate,
												lfirst(current_fargs),
												input_typeids[i],
												function_typeids[i]);
		}
	}
}

/*
 ** setup_tlist 
 **		Build a tlist that says which attribute to project to.
 **		This routine is called by ParseFuncOrColumn() to set up a target list
 **		on a tuple parameter or return value.  Due to a bug in 4.0,
 **		it's not possible to refer to system attributes in this case.
 */
static List *
setup_tlist(char *attname, Oid relid)
{
	TargetEntry *tle;
	Resdom	   *resnode;
	Var		   *varnode;
	Oid			typeid;
	int32		type_mod;
	int			attno;

	attno = get_attnum(relid, attname);
	if (attno < 0)
		elog(ERROR, "Cannot reference attribute '%s'"
			 " of tuple params/return values for functions", attname);

	typeid = get_atttype(relid, attno);
	type_mod = get_atttypmod(relid, attno);

	resnode = makeResdom(1,
						 typeid,
						 type_mod,
						 get_attname(relid, attno),
						 0,
						 (Oid) 0,
						 0);
	varnode = makeVar(-1, attno, typeid, type_mod, 0, -1, attno);

	tle = makeTargetEntry(resnode, (Node *) varnode);
	return lcons(tle, NIL);
}

/*
 ** setup_base_tlist 
 **		Build a tlist that extracts a base type from the tuple
 **		returned by the executor.
 */
static List *
setup_base_tlist(Oid typeid)
{
	TargetEntry *tle;
	Resdom	   *resnode;
	Var		   *varnode;

	resnode = makeResdom(1,
						 typeid,
						 -1,
						 "<noname>",
						 0,
						 (Oid) 0,
						 0);
	varnode = makeVar(-1, 1, typeid, -1, 0, -1, 1);
	tle = makeTargetEntry(resnode, (Node *) varnode);

	return lcons(tle, NIL);
}

/*
 * ParseComplexProjection -
 *	  handles function calls with a single argument that is of complex type.
 *	  This routine returns NULL if it can't handle the projection (eg. sets).
 */
static Node *
ParseComplexProjection(ParseState *pstate,
					   char *funcname,
					   Node *first_arg,
					   bool *attisset)
{
	Oid			argtype;
	Oid			argrelid;
	Relation	rd;
	Oid			relid;
	int			attnum;

	switch (nodeTag(first_arg))
	{
		case T_Iter:
			{
				Func	   *func;
				Iter	   *iter;

				iter = (Iter *) first_arg;
				func = (Func *) ((Expr *) iter->iterexpr)->oper;
				argtype = funcid_get_rettype(func->funcid);
				argrelid = typeidTypeRelid(argtype);
				if (argrelid &&
					((attnum = get_attnum(argrelid, funcname))
					 != InvalidAttrNumber))
				{

					/*
					 * the argument is a function returning a tuple, so
					 * funcname may be a projection
					 */

					/* add a tlist to the func node and return the Iter */
					rd = heap_openr(typeidTypeName(argtype));
					if (RelationIsValid(rd))
					{
						relid = RelationGetRelid(rd);
						heap_close(rd);
					}
					if (RelationIsValid(rd))
					{
						func->func_tlist = setup_tlist(funcname, argrelid);
						iter->itertype = attnumTypeId(rd, attnum);
						return (Node *) iter;
					}
					else
					{
						elog(ERROR, "Function '%s' has bad return type %d",
							 funcname, argtype);
					}
				}
				else
				{
					/* drop through */
					;
				}
				break;
			}
		case T_Var:
			{

				/*
				 * The argument is a set, so this is either a projection
				 * or a function call on this set.
				 */
				*attisset = true;
				break;
			}
		case T_Expr:
			{
				Expr	   *expr = (Expr *) first_arg;
				Func	   *funcnode;

				if (expr->opType != FUNC_EXPR)
					break;

				funcnode = (Func *) expr->oper;
				argtype = funcid_get_rettype(funcnode->funcid);
				argrelid = typeidTypeRelid(argtype);

				/*
				 * the argument is a function returning a tuple, so
				 * funcname may be a projection
				 */
				if (argrelid &&
					(attnum = get_attnum(argrelid, funcname))
					!= InvalidAttrNumber)
				{

					/* add a tlist to the func node */
					rd = heap_openr(typeidTypeName(argtype));
					if (RelationIsValid(rd))
					{
						relid = RelationGetRelid(rd);
						heap_close(rd);
					}
					if (RelationIsValid(rd))
					{
						Expr	   *newexpr;

						funcnode->func_tlist = setup_tlist(funcname, argrelid);
						funcnode->functype = attnumTypeId(rd, attnum);

						newexpr = makeNode(Expr);
						newexpr->typeOid = funcnode->functype;
						newexpr->opType = FUNC_EXPR;
						newexpr->oper = (Node *) funcnode;
						newexpr->args = expr->args;

						return (Node *) newexpr;
					}

				}

				break;
			}
		case T_Param:
			{
				Param	   *param = (Param *) first_arg;

				/*
				 * If the Param is a complex type, this could be a
				 * projection
				 */
				rd = heap_openr(typeidTypeName(param->paramtype));
				if (RelationIsValid(rd))
				{
					relid = RelationGetRelid(rd);
					heap_close(rd);
					if ((attnum = get_attnum(relid, funcname))
						!= InvalidAttrNumber)
					{
						param->paramtype = attnumTypeId(rd, attnum);
						param->param_tlist = setup_tlist(funcname, relid);
						return (Node *) param;
					}
				}
				break;
			}
		default:
			break;
	}

	return NULL;
}

/*
 * Error message when function lookup fails that gives details of the
 * argument types
 */
void
func_error(char *caller, char *funcname, int nargs, Oid *argtypes, char *msg)
{
	char		p[(NAMEDATALEN + 2) * MAXFMGRARGS],
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
			 funcname, p, ((msg != NULL) ? "\n\t" : ""), ((msg != NULL) ? msg : ""));
	}
	else
	{
		elog(ERROR, "%s: function '%s(%s)' does not exist%s%s",
			 caller, funcname, p, ((msg != NULL) ? "\n\t" : ""), ((msg != NULL) ? msg : ""));
	}
}
