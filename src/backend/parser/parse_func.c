/*-------------------------------------------------------------------------
 *
 * parse_func.c
 *		handle function calls in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/parse_func.c,v 1.10 1998/02/02 02:12:34 scrappy Exp $
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
static Oid ** argtype_inherit(int nargs, Oid *oid_array);
static bool can_coerce(int nargs, Oid *input_typeids, Oid *func_typeids);
static int find_inheritors(Oid relid, Oid **supervec);
static CandidateList func_get_candidates(char *funcname, int nargs);
static bool func_get_detail(char *funcname,
					int nargs,
					Oid *oid_array,
					Oid *funcid,	/* return value */
					Oid *rettype,	/* return value */
					bool *retset,	/* return value */
					Oid **true_typeids);
static Oid * func_select_candidate(int nargs,
						  Oid *input_typeids,
						  CandidateList candidates);
static Oid funcid_get_rettype(Oid funcid);
static Oid **gen_cross_product(InhPaths *arginh, int nargs);
static void make_arguments(int nargs,
				   List *fargs,
				   Oid *input_typeids,
				   Oid *function_typeids);
static int match_argtypes(int nargs,
				   Oid *input_typeids,
				   CandidateList function_typeids,
				   CandidateList *candidates);
static List *setup_tlist(char *attname, Oid relid);
static List *setup_base_tlist(Oid typeid);

#define ISCOMPLEX(type) (typeidTypeRelid(type) ? true : false)

#define MAXFARGS 8				/* max # args to a c or postquel function */

typedef struct _SuperQE
{
	Oid			sqe_relid;
} SuperQE;

/*
 ** ParseNestedFuncOrColumn --
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

	return (retval);
}

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
	bool		exists;
	bool		attisset = false;
	Oid			toid = (Oid) 0;
	Expr	   *expr;

	if (fargs)
	{
		first_arg = lfirst(fargs);
		if (first_arg == NULL)
			elog(ERROR, "function '%s' does not allow NULL input", funcname);
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
				Oid			dummyTypeId;

				return ((Node *) make_var(pstate,
									   relid,
									   refname,
									   funcname,
									   &dummyTypeId));
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
					elog(ERROR,
						 "Type '%s' is not a relation type",
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
			Oid			basetype;

			/*
			 * the aggregate COUNT is a special case, ignore its base
			 * type.  Treat it as zero
			 */
			if (strcmp(funcname, "count") == 0)
				basetype = 0;
			else
				basetype = exprType(lfirst(fargs));
			if (SearchSysCacheTuple(AGGNAME,
									PointerGetDatum(funcname),
									ObjectIdGetDatum(basetype),
									0, 0))
				return (Node *)ParseAgg(pstate, funcname, basetype,
										fargs, precedence);
		}
	}


	/*
	 * * If we dropped through to here it's really a function (or a set,
	 * which * is implemented as a function.) * extract arg type info and
	 * transform relation name arguments into * varnodes of the
	 * appropriate form.
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
			lfirst(fargs) =
				makeVar(vnum, 0, toid, 0, vnum, 0);
		}
		else if (!attisset)
		{						/* set functions don't have parameters */

			/*
			 * any functiona args which are typed "unknown", but aren't
			 * constants, we don't know what to do with, because we can't
			 * cast them	- jolly
			 */
			if (exprType(pair) == UNKNOWNOID &&
				!IsA(pair, Const))
				elog(ERROR, "ParseFuncOrColumn: no function named '%s' that takes in an unknown type as argument #%d", funcname, nargs);
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
		funcid = SetEvalRegProcedure;
		rettype = toid;
		retset = true;
		true_oid_array = oid_array;
		exists = true;
	}
	else
	{
		exists = func_get_detail(funcname, nargs, oid_array, &funcid,
								 &rettype, &retset, &true_oid_array);
	}

	if (!exists)
		elog(ERROR, "no such attribute or function '%s'", funcname);

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
	make_arguments(nargs, fargs, oid_array, true_oid_array);

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
			funcnode->func_tlist =
				expandAll(pstate, relname, refname, curr_resno);
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
	if (funcid == SeqNextValueRegProcedure ||
		funcid == SeqCurrValueRegProcedure)
	{
		Const	   *seq;
		char	   *seqrel;
		text	   *seqname;
		int32		aclcheck_result = -1;
		extern text *lower (text *string);

		Assert(length(fargs) == 1);
		seq = (Const *) lfirst(fargs);
		if (!IsA((Node *) seq, Const))
			elog(ERROR, "%s: only constant sequence names are acceptable", funcname);
		seqname = lower ((text*)DatumGetPointer(seq->constvalue));
		pfree (DatumGetPointer(seq->constvalue));
		seq->constvalue = PointerGetDatum (seqname);
		seqrel = textout(seqname);

		if ((aclcheck_result = pg_aclcheck(seqrel, GetPgUserName(),
			   ((funcid == SeqNextValueRegProcedure) ? ACL_WR : ACL_RD)))
			!= ACLCHECK_OK)
			elog(ERROR, "%s.%s: %s",
			  seqrel, funcname, aclcheck_error_strings[aclcheck_result]);

		pfree(seqrel);

		if (funcid == SeqNextValueRegProcedure && pstate->p_in_where_clause)
			elog(ERROR, "nextval of a sequence in WHERE disallowed");
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

	return (retval);
}

static Oid
funcid_get_rettype(Oid funcid)
{
	HeapTuple	func_tuple = NULL;
	Oid			funcrettype = (Oid) 0;

	func_tuple = SearchSysCacheTuple(PROOID, ObjectIdGetDatum(funcid),
									 0, 0, 0);

	if (!HeapTupleIsValid(func_tuple))
		elog(ERROR, "function  %d does not exist", funcid);

	funcrettype = (Oid)
		((Form_pg_proc) GETSTRUCT(func_tuple))->prorettype;

	return (funcrettype);
}

/*
 * get a list of all argument type vectors for which a function named
 * funcname taking nargs arguments exists
 */
static CandidateList
func_get_candidates(char *funcname, int nargs)
{
	Relation	heapRelation;
	Relation	idesc;
	ScanKeyData skey;
	HeapTuple	tuple;
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	Buffer		buffer;
	Form_pg_proc pgProcP;
	bool		bufferUsed = FALSE;
	CandidateList candidates = NULL;
	CandidateList current_candidate;
	int			i;

	heapRelation = heap_openr(ProcedureRelationName);
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) NameEqualRegProcedure,
						   (Datum) funcname);

	idesc = index_openr(ProcedureNameIndex);

	sd = index_beginscan(idesc, false, 1, &skey);

	do
	{
		tuple = (HeapTuple) NULL;
		if (bufferUsed)
		{
			ReleaseBuffer(buffer);
			bufferUsed = FALSE;
		}

		indexRes = index_getnext(sd, ForwardScanDirection);
		if (indexRes)
		{
			ItemPointer iptr;

			iptr = &indexRes->heap_iptr;
			tuple = heap_fetch(heapRelation, false, iptr, &buffer);
			pfree(indexRes);
			if (HeapTupleIsValid(tuple))
			{
				pgProcP = (Form_pg_proc) GETSTRUCT(tuple);
				bufferUsed = TRUE;
				if (pgProcP->pronargs == nargs)
				{
					current_candidate = (CandidateList)
						palloc(sizeof(struct _CandidateList));
					current_candidate->args = (Oid *)
						palloc(8 * sizeof(Oid));
					MemSet(current_candidate->args, 0, 8 * sizeof(Oid));
					for (i = 0; i < nargs; i++)
						current_candidate->args[i] =
							pgProcP->proargtypes[i];

					current_candidate->next = candidates;
					candidates = current_candidate;
				}
			}
		}
	} while (indexRes);

	index_endscan(sd);
	index_close(idesc);
	heap_close(heapRelation);

	return candidates;
}

/*
 * can input_typeids be coerced to func_typeids?
 */
static bool
can_coerce(int nargs, Oid *input_typeids, Oid *func_typeids)
{
	int			i;
	Type		tp;

	/*
	 * right now, we only coerce "unknown", and we cannot coerce it to a
	 * relation type
	 */
	for (i = 0; i < nargs; i++)
	{
		if (input_typeids[i] != func_typeids[i])
		{
			if ((input_typeids[i] == BPCHAROID && func_typeids[i] == TEXTOID) ||
				(input_typeids[i] == BPCHAROID && func_typeids[i] == VARCHAROID) ||
				(input_typeids[i] == VARCHAROID && func_typeids[i] == TEXTOID) ||
				(input_typeids[i] == VARCHAROID && func_typeids[i] == BPCHAROID) ||
			(input_typeids[i] == CASHOID && func_typeids[i] == INT4OID) ||
			 (input_typeids[i] == INT4OID && func_typeids[i] == CASHOID))
				;				/* these are OK */
			else if (input_typeids[i] != UNKNOWNOID || func_typeids[i] == 0)
				return false;

			tp = typeidType(input_typeids[i]);
			if (typeTypeFlag(tp) == 'c')
				return false;
		}
	}

	return true;
}

/*
 * given a list of possible typeid arrays to a function and an array of
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
		if (can_coerce(nargs, input_typeids, current_typeids))
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
}

/*
 * given the input argtype array and more than one candidate
 * for the function argtype array, attempt to resolve the conflict.
 * returns the selected argtype array if the conflict can be resolved,
 * otherwise returns NULL
 */
static Oid *
func_select_candidate(int nargs,
					  Oid *input_typeids,
					  CandidateList candidates)
{
	/* XXX no conflict resolution implemeneted yet */
	return (NULL);
}

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

	/*
	 * attempt to find named function in the system catalogs with
	 * arguments exactly as specified - so that the normal case is just as
	 * quick as before
	 */
	ftup = SearchSysCacheTuple(PRONAME,
							   PointerGetDatum(funcname),
							   Int32GetDatum(nargs),
							   PointerGetDatum(oid_array),
							   0);
	*true_typeids = oid_array;

	/*
	 * If an exact match isn't found : 1) get a vector of all possible
	 * input arg type arrays constructed from the superclasses of the
	 * original input arg types 2) get a list of all possible argument
	 * type arrays to the function with given name and number of arguments
	 * 3) for each input arg type array from vector #1 : a) find how many
	 * of the function arg type arrays from list #2 it can be coerced to
	 * b) - if the answer is one, we have our function - if the answer is
	 * more than one, attempt to resolve the conflict - if the answer is
	 * zero, try the next array from vector #1
	 */
	if (!HeapTupleIsValid(ftup))
	{
		function_typeids = func_get_candidates(funcname, nargs);

		if (function_typeids != NULL)
		{
			int			ncandidates = 0;

			input_typeid_vector = argtype_inherit(nargs, oid_array);
			current_input_typeids = oid_array;

			do
			{
				ncandidates = match_argtypes(nargs, current_input_typeids,
											 function_typeids,
											 &current_function_typeids);
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
				else if (ncandidates > 1)
				{
					*true_typeids =
						func_select_candidate(nargs,
											  current_input_typeids,
											  current_function_typeids);
					if (*true_typeids == NULL)
					{
						elog(NOTICE, "there is more than one function named \"%s\"",
							 funcname);
						elog(NOTICE, "that satisfies the given argument types. you will have to");
						elog(NOTICE, "retype your query using explicit typecasts.");
						func_error("func_get_detail", funcname, nargs, oid_array);
					}
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
			while (current_input_typeids !=
				   InvalidOid && ncandidates == 0);
		}
	}

	if (!HeapTupleIsValid(ftup))
	{
		Type		tp;

		if (nargs == 1)
		{
			tp = typeidType(oid_array[0]);
			if (typeTypeFlag(tp) == 'c')
				elog(ERROR, "no such attribute or function \"%s\"",
					 funcname);
		}
		func_error("func_get_detail", funcname, nargs, oid_array);
	}
	else
	{
		pform = (Form_pg_proc) GETSTRUCT(ftup);
		*funcid = ftup->t_oid;
		*rettype = pform->prorettype;
		*retset = pform->proretset;

		return (true);
	}
/* shouldn't reach here */
	return (false);

}

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
			{
				arginh[i].nsupers = find_inheritors(relid, &(arginh[i].supervec));
			}
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
	return (gen_cross_product(arginh, nargs));
}

static int find_inheritors(Oid relid, Oid **supervec)
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
	Buffer		buf;
	Datum		d;
	bool		newrelid;
	char		isNull;

	nvisited = 0;
	queue = DLNewList();
	visited = DLNewList();


	inhrel = heap_openr(InheritsRelationName);
	RelationSetLockForRead(inhrel);
	inhtupdesc = RelationGetTupleDescriptor(inhrel);

	/*
	 * Use queue to do a breadth-first traversal of the inheritance graph
	 * from the relid supplied up to the root.
	 */
	do
	{
		ScanKeyEntryInitialize(&skey, 0x0, Anum_pg_inherits_inhrel,
							   ObjectIdEqualRegProcedure,
							   ObjectIdGetDatum(relid));

		inhscan = heap_beginscan(inhrel, 0, false, 1, &skey);

		while (HeapTupleIsValid(inhtup = heap_getnext(inhscan, 0, &buf)))
		{
			qentry = (SuperQE *) palloc(sizeof(SuperQE));

			d = fastgetattr(inhtup, Anum_pg_inherits_inhparent,
							inhtupdesc, &isNull);
			qentry->sqe_relid = DatumGetObjectId(d);

			/* put this one on the queue */
			DLAddTail(queue, DLNewElem(qentry));

			ReleaseBuffer(buf);
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
				elog(ERROR, "relid %d does not exist", qentry->sqe_relid);
			qentry->sqe_relid = typeTypeId(typenameType(RelationGetRelationName(rd)->data));
			heap_close(rd);

			DLAddTail(visited, qe);

			nvisited++;
		}
	} while (qentry != (SuperQE *) NULL);

	RelationUnsetLockForRead(inhrel);
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
	{
		*supervec = (Oid *) NULL;
	}

	return (nvisited);
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
			return (result);
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
 ** make_arguments --
 **   Given the number and types of arguments to a function, and the
 **   actual arguments and argument types, do the necessary typecasting.
 */
static void
make_arguments(int nargs,
			   List *fargs,
			   Oid *input_typeids,
			   Oid *function_typeids)
{

	/*
	 * there are two ways an input typeid can differ from a function
	 * typeid : either the input type inherits the function type, so no
	 * typecasting is necessary, or the input type can be typecast into
	 * the function type. right now, we only typecast unknowns, and that
	 * is all we check for.
	 */

	List	   *current_fargs;
	int			i;

	for (i = 0, current_fargs = fargs;
		 i < nargs;
		 i++, current_fargs = lnext(current_fargs))
	{

		if (input_typeids[i] == UNKNOWNOID && function_typeids[i] != InvalidOid)
		{
			lfirst(current_fargs) =
				parser_typecast2(lfirst(current_fargs),
								 input_typeids[i],
								 typeidType(function_typeids[i]),
								 -1);
		}
	}
}

/*
 ** setup_tlist --
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
	int			attno;

	attno = get_attnum(relid, attname);
	if (attno < 0)
		elog(ERROR, "cannot reference attribute '%s' of tuple params/return values for functions", attname);

	typeid = get_atttype(relid, attno);
	resnode = makeResdom(1,
						 typeid,
						 typeLen(typeidType(typeid)),
						 get_attname(relid, attno),
						 0,
						 (Oid) 0,
						 0);
	varnode = makeVar(-1, attno, typeid, 0, -1, attno);

	tle = makeNode(TargetEntry);
	tle->resdom = resnode;
	tle->expr = (Node *) varnode;
	return (lcons(tle, NIL));
}

/*
 ** setup_base_tlist --
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
						 typeLen(typeidType(typeid)),
						 "<noname>",
						 0,
						 (Oid) 0,
						 0);
	varnode = makeVar(-1, 1, typeid, 0, -1, 1);
	tle = makeNode(TargetEntry);
	tle->resdom = resnode;
	tle->expr = (Node *) varnode;

	return (lcons(tle, NIL));
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
	Name		relname;
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
						relid = RelationGetRelationId(rd);
						relname = RelationGetRelationName(rd);
						heap_close(rd);
					}
					if (RelationIsValid(rd))
					{
						func->func_tlist =
							setup_tlist(funcname, argrelid);
						iter->itertype = attnumTypeId(rd, attnum);
						return ((Node *) iter);
					}
					else
					{
						elog(ERROR,
							 "Function '%s' has bad returntype %d",
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
						relid = RelationGetRelationId(rd);
						relname = RelationGetRelationName(rd);
						heap_close(rd);
					}
					if (RelationIsValid(rd))
					{
						Expr	   *newexpr;

						funcnode->func_tlist =
							setup_tlist(funcname, argrelid);
						funcnode->functype = attnumTypeId(rd, attnum);

						newexpr = makeNode(Expr);
						newexpr->typeOid = funcnode->functype;
						newexpr->opType = FUNC_EXPR;
						newexpr->oper = (Node *) funcnode;
						newexpr->args = lcons(first_arg, NIL);

						return ((Node *) newexpr);
					}

				}

				elog(ERROR, "Function '%s' has bad returntype %d",
					 funcname, argtype);
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
					relid = RelationGetRelationId(rd);
					relname = RelationGetRelationName(rd);
					heap_close(rd);
				}
				if (RelationIsValid(rd) &&
					(attnum = get_attnum(relid, funcname))
					!= InvalidAttrNumber)
				{

					param->paramtype = attnumTypeId(rd, attnum);
					param->param_tlist = setup_tlist(funcname, relid);
					return ((Node *) param);
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
func_error(char *caller, char *funcname, int nargs, Oid *argtypes)
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

	elog(ERROR, "%s: function %s(%s) does not exist", caller, funcname, p);
}
