/*-------------------------------------------------------------------------
 *
 * lsyscache.c
 *	  Convenience routines for common queries in the system catalog cache.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/lsyscache.c
 *
 * NOTES
 *	  Eventually, the index information should go through here, too.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "bootstrap/bootstrap.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* Hook for plugins to get control in get_attavgwidth() */
get_attavgwidth_hook_type get_attavgwidth_hook = NULL;


/*				---------- AMOP CACHES ----------						 */

/*
 * op_in_opfamily
 *
 *		Return t iff operator 'opno' is in operator family 'opfamily'.
 *
 * This function only considers search operators, not ordering operators.
 */
bool
op_in_opfamily(Oid opno, Oid opfamily)
{
	return SearchSysCacheExists3(AMOPOPID,
								 ObjectIdGetDatum(opno),
								 CharGetDatum(AMOP_SEARCH),
								 ObjectIdGetDatum(opfamily));
}

/*
 * get_op_opfamily_strategy
 *
 *		Get the operator's strategy number within the specified opfamily,
 *		or 0 if it's not a member of the opfamily.
 *
 * This function only considers search operators, not ordering operators.
 */
int
get_op_opfamily_strategy(Oid opno, Oid opfamily)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	int			result;

	tp = SearchSysCache3(AMOPOPID,
						 ObjectIdGetDatum(opno),
						 CharGetDatum(AMOP_SEARCH),
						 ObjectIdGetDatum(opfamily));
	if (!HeapTupleIsValid(tp))
		return 0;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopstrategy;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_op_opfamily_sortfamily
 *
 *		If the operator is an ordering operator within the specified opfamily,
 *		return its amopsortfamily OID; else return InvalidOid.
 */
Oid
get_op_opfamily_sortfamily(Oid opno, Oid opfamily)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	Oid			result;

	tp = SearchSysCache3(AMOPOPID,
						 ObjectIdGetDatum(opno),
						 CharGetDatum(AMOP_ORDER),
						 ObjectIdGetDatum(opfamily));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopsortfamily;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_op_opfamily_properties
 *
 *		Get the operator's strategy number and declared input data types
 *		within the specified opfamily.
 *
 * Caller should already have verified that opno is a member of opfamily,
 * therefore we raise an error if the tuple is not found.
 */
void
get_op_opfamily_properties(Oid opno, Oid opfamily, bool ordering_op,
						   int *strategy,
						   Oid *lefttype,
						   Oid *righttype)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;

	tp = SearchSysCache3(AMOPOPID,
						 ObjectIdGetDatum(opno),
						 CharGetDatum(ordering_op ? AMOP_ORDER : AMOP_SEARCH),
						 ObjectIdGetDatum(opfamily));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "operator %u is not a member of opfamily %u",
			 opno, opfamily);
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	*strategy = amop_tup->amopstrategy;
	*lefttype = amop_tup->amoplefttype;
	*righttype = amop_tup->amoprighttype;
	ReleaseSysCache(tp);
}

/*
 * get_opfamily_member
 *		Get the OID of the operator that implements the specified strategy
 *		with the specified datatypes for the specified opfamily.
 *
 * Returns InvalidOid if there is no pg_amop entry for the given keys.
 */
Oid
get_opfamily_member(Oid opfamily, Oid lefttype, Oid righttype,
					int16 strategy)
{
	HeapTuple	tp;
	Form_pg_amop amop_tup;
	Oid			result;

	tp = SearchSysCache4(AMOPSTRATEGY,
						 ObjectIdGetDatum(opfamily),
						 ObjectIdGetDatum(lefttype),
						 ObjectIdGetDatum(righttype),
						 Int16GetDatum(strategy));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amop_tup = (Form_pg_amop) GETSTRUCT(tp);
	result = amop_tup->amopopr;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_ordering_op_properties
 *		Given the OID of an ordering operator (a btree "<" or ">" operator),
 *		determine its opfamily, its declared input datatype, and its
 *		strategy number (BTLessStrategyNumber or BTGreaterStrategyNumber).
 *
 * Returns TRUE if successful, FALSE if no matching pg_amop entry exists.
 * (This indicates that the operator is not a valid ordering operator.)
 *
 * Note: the operator could be registered in multiple families, for example
 * if someone were to build a "reverse sort" opfamily.	This would result in
 * uncertainty as to whether "ORDER BY USING op" would default to NULLS FIRST
 * or NULLS LAST, as well as inefficient planning due to failure to match up
 * pathkeys that should be the same.  So we want a determinate result here.
 * Because of the way the syscache search works, we'll use the interpretation
 * associated with the opfamily with smallest OID, which is probably
 * determinate enough.	Since there is no longer any particularly good reason
 * to build reverse-sort opfamilies, it doesn't seem worth expending any
 * additional effort on ensuring consistency.
 */
bool
get_ordering_op_properties(Oid opno,
						   Oid *opfamily, Oid *opcintype, int16 *strategy)
{
	bool		result = false;
	CatCList   *catlist;
	int			i;

	/* ensure outputs are initialized on failure */
	*opfamily = InvalidOid;
	*opcintype = InvalidOid;
	*strategy = 0;

	/*
	 * Search pg_amop to see if the target operator is registered as the "<"
	 * or ">" operator of any btree opfamily.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* must be btree */
		if (aform->amopmethod != BTREE_AM_OID)
			continue;

		if (aform->amopstrategy == BTLessStrategyNumber ||
			aform->amopstrategy == BTGreaterStrategyNumber)
		{
			/* Found it ... should have consistent input types */
			if (aform->amoplefttype == aform->amoprighttype)
			{
				/* Found a suitable opfamily, return info */
				*opfamily = aform->amopfamily;
				*opcintype = aform->amoplefttype;
				*strategy = aform->amopstrategy;
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_sort_function_for_ordering_op
 *		Get the OID of the datatype-specific btree sort support function,
 *		or if there is none, the btree comparison function,
 *		associated with an ordering operator (a "<" or ">" operator).
 *
 * *sortfunc receives the support or comparison function OID.
 * *issupport is set TRUE if it's a support func, FALSE if a comparison func.
 * *reverse is set FALSE if the operator is "<", TRUE if it's ">"
 * (indicating that comparison results must be negated before use).
 *
 * Returns TRUE if successful, FALSE if no btree function can be found.
 * (This indicates that the operator is not a valid ordering operator.)
 */
bool
get_sort_function_for_ordering_op(Oid opno, Oid *sortfunc,
								  bool *issupport, bool *reverse)
{
	Oid			opfamily;
	Oid			opcintype;
	int16		strategy;

	/* Find the operator in pg_amop */
	if (get_ordering_op_properties(opno,
								   &opfamily, &opcintype, &strategy))
	{
		/* Found a suitable opfamily, get matching support function */
		*sortfunc = get_opfamily_proc(opfamily,
									  opcintype,
									  opcintype,
									  BTSORTSUPPORT_PROC);
		if (OidIsValid(*sortfunc))
			*issupport = true;
		else
		{
			/* opfamily doesn't provide sort support, get comparison func */
			*sortfunc = get_opfamily_proc(opfamily,
										  opcintype,
										  opcintype,
										  BTORDER_PROC);
			if (!OidIsValid(*sortfunc)) /* should not happen */
				elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
					 BTORDER_PROC, opcintype, opcintype, opfamily);
			*issupport = false;
		}
		*reverse = (strategy == BTGreaterStrategyNumber);
		return true;
	}

	/* ensure outputs are set on failure */
	*sortfunc = InvalidOid;
	*issupport = false;
	*reverse = false;
	return false;
}

/*
 * get_equality_op_for_ordering_op
 *		Get the OID of the datatype-specific btree equality operator
 *		associated with an ordering operator (a "<" or ">" operator).
 *
 * If "reverse" isn't NULL, also set *reverse to FALSE if the operator is "<",
 * TRUE if it's ">"
 *
 * Returns InvalidOid if no matching equality operator can be found.
 * (This indicates that the operator is not a valid ordering operator.)
 */
Oid
get_equality_op_for_ordering_op(Oid opno, bool *reverse)
{
	Oid			result = InvalidOid;
	Oid			opfamily;
	Oid			opcintype;
	int16		strategy;

	/* Find the operator in pg_amop */
	if (get_ordering_op_properties(opno,
								   &opfamily, &opcintype, &strategy))
	{
		/* Found a suitable opfamily, get matching equality operator */
		result = get_opfamily_member(opfamily,
									 opcintype,
									 opcintype,
									 BTEqualStrategyNumber);
		if (reverse)
			*reverse = (strategy == BTGreaterStrategyNumber);
	}

	return result;
}

/*
 * get_ordering_op_for_equality_op
 *		Get the OID of a datatype-specific btree ordering operator
 *		associated with an equality operator.  (If there are multiple
 *		possibilities, assume any one will do.)
 *
 * This function is used when we have to sort data before unique-ifying,
 * and don't much care which sorting op is used as long as it's compatible
 * with the intended equality operator.  Since we need a sorting operator,
 * it should be single-data-type even if the given operator is cross-type.
 * The caller specifies whether to find an op for the LHS or RHS data type.
 *
 * Returns InvalidOid if no matching ordering operator can be found.
 */
Oid
get_ordering_op_for_equality_op(Oid opno, bool use_lhs_type)
{
	Oid			result = InvalidOid;
	CatCList   *catlist;
	int			i;

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any btree opfamily.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* must be btree */
		if (aform->amopmethod != BTREE_AM_OID)
			continue;

		if (aform->amopstrategy == BTEqualStrategyNumber)
		{
			/* Found a suitable opfamily, get matching ordering operator */
			Oid			typid;

			typid = use_lhs_type ? aform->amoplefttype : aform->amoprighttype;
			result = get_opfamily_member(aform->amopfamily,
										 typid, typid,
										 BTLessStrategyNumber);
			if (OidIsValid(result))
				break;
			/* failure probably shouldn't happen, but keep looking if so */
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_mergejoin_opfamilies
 *		Given a putatively mergejoinable operator, return a list of the OIDs
 *		of the btree opfamilies in which it represents equality.
 *
 * It is possible (though at present unusual) for an operator to be equality
 * in more than one opfamily, hence the result is a list.  This also lets us
 * return NIL if the operator is not found in any opfamilies.
 *
 * The planner currently uses simple equal() tests to compare the lists
 * returned by this function, which makes the list order relevant, though
 * strictly speaking it should not be.	Because of the way syscache list
 * searches are handled, in normal operation the result will be sorted by OID
 * so everything works fine.  If running with system index usage disabled,
 * the result ordering is unspecified and hence the planner might fail to
 * recognize optimization opportunities ... but that's hardly a scenario in
 * which performance is good anyway, so there's no point in expending code
 * or cycles here to guarantee the ordering in that case.
 */
List *
get_mergejoin_opfamilies(Oid opno)
{
	List	   *result = NIL;
	CatCList   *catlist;
	int			i;

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any btree opfamily.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		/* must be btree equality */
		if (aform->amopmethod == BTREE_AM_OID &&
			aform->amopstrategy == BTEqualStrategyNumber)
			result = lappend_oid(result, aform->amopfamily);
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_compatible_hash_operators
 *		Get the OID(s) of hash equality operator(s) compatible with the given
 *		operator, but operating on its LHS and/or RHS datatype.
 *
 * An operator for the LHS type is sought and returned into *lhs_opno if
 * lhs_opno isn't NULL.  Similarly, an operator for the RHS type is sought
 * and returned into *rhs_opno if rhs_opno isn't NULL.
 *
 * If the given operator is not cross-type, the results should be the same
 * operator, but in cross-type situations they will be different.
 *
 * Returns true if able to find the requested operator(s), false if not.
 * (This indicates that the operator should not have been marked oprcanhash.)
 */
bool
get_compatible_hash_operators(Oid opno,
							  Oid *lhs_opno, Oid *rhs_opno)
{
	bool		result = false;
	CatCList   *catlist;
	int			i;

	/* Ensure output args are initialized on failure */
	if (lhs_opno)
		*lhs_opno = InvalidOid;
	if (rhs_opno)
		*rhs_opno = InvalidOid;

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any hash opfamily.  If the operator is registered in
	 * multiple opfamilies, assume we can use any one.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopmethod == HASH_AM_OID &&
			aform->amopstrategy == HTEqualStrategyNumber)
		{
			/* No extra lookup needed if given operator is single-type */
			if (aform->amoplefttype == aform->amoprighttype)
			{
				if (lhs_opno)
					*lhs_opno = opno;
				if (rhs_opno)
					*rhs_opno = opno;
				result = true;
				break;
			}

			/*
			 * Get the matching single-type operator(s).  Failure probably
			 * shouldn't happen --- it implies a bogus opfamily --- but
			 * continue looking if so.
			 */
			if (lhs_opno)
			{
				*lhs_opno = get_opfamily_member(aform->amopfamily,
												aform->amoplefttype,
												aform->amoplefttype,
												HTEqualStrategyNumber);
				if (!OidIsValid(*lhs_opno))
					continue;
				/* Matching LHS found, done if caller doesn't want RHS */
				if (!rhs_opno)
				{
					result = true;
					break;
				}
			}
			if (rhs_opno)
			{
				*rhs_opno = get_opfamily_member(aform->amopfamily,
												aform->amoprighttype,
												aform->amoprighttype,
												HTEqualStrategyNumber);
				if (!OidIsValid(*rhs_opno))
				{
					/* Forget any LHS operator from this opfamily */
					if (lhs_opno)
						*lhs_opno = InvalidOid;
					continue;
				}
				/* Matching RHS found, so done */
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_op_hash_functions
 *		Get the OID(s) of hash support function(s) compatible with the given
 *		operator, operating on its LHS and/or RHS datatype as required.
 *
 * A function for the LHS type is sought and returned into *lhs_procno if
 * lhs_procno isn't NULL.  Similarly, a function for the RHS type is sought
 * and returned into *rhs_procno if rhs_procno isn't NULL.
 *
 * If the given operator is not cross-type, the results should be the same
 * function, but in cross-type situations they will be different.
 *
 * Returns true if able to find the requested function(s), false if not.
 * (This indicates that the operator should not have been marked oprcanhash.)
 */
bool
get_op_hash_functions(Oid opno,
					  RegProcedure *lhs_procno, RegProcedure *rhs_procno)
{
	bool		result = false;
	CatCList   *catlist;
	int			i;

	/* Ensure output args are initialized on failure */
	if (lhs_procno)
		*lhs_procno = InvalidOid;
	if (rhs_procno)
		*rhs_procno = InvalidOid;

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any hash opfamily.  If the operator is registered in
	 * multiple opfamilies, assume we can use any one.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopmethod == HASH_AM_OID &&
			aform->amopstrategy == HTEqualStrategyNumber)
		{
			/*
			 * Get the matching support function(s).  Failure probably
			 * shouldn't happen --- it implies a bogus opfamily --- but
			 * continue looking if so.
			 */
			if (lhs_procno)
			{
				*lhs_procno = get_opfamily_proc(aform->amopfamily,
												aform->amoplefttype,
												aform->amoplefttype,
												HASHPROC);
				if (!OidIsValid(*lhs_procno))
					continue;
				/* Matching LHS found, done if caller doesn't want RHS */
				if (!rhs_procno)
				{
					result = true;
					break;
				}
				/* Only one lookup needed if given operator is single-type */
				if (aform->amoplefttype == aform->amoprighttype)
				{
					*rhs_procno = *lhs_procno;
					result = true;
					break;
				}
			}
			if (rhs_procno)
			{
				*rhs_procno = get_opfamily_proc(aform->amopfamily,
												aform->amoprighttype,
												aform->amoprighttype,
												HASHPROC);
				if (!OidIsValid(*rhs_procno))
				{
					/* Forget any LHS function from this opfamily */
					if (lhs_procno)
						*lhs_procno = InvalidOid;
					continue;
				}
				/* Matching RHS found, so done */
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

/*
 * get_op_btree_interpretation
 *		Given an operator's OID, find out which btree opfamilies it belongs to,
 *		and what properties it has within each one.  The results are returned
 *		as a palloc'd list of OpBtreeInterpretation structs.
 *
 * In addition to the normal btree operators, we consider a <> operator to be
 * a "member" of an opfamily if its negator is an equality operator of the
 * opfamily.  ROWCOMPARE_NE is returned as the strategy number for this case.
 */
List *
get_op_btree_interpretation(Oid opno)
{
	List	   *result = NIL;
	OpBtreeInterpretation *thisresult;
	CatCList   *catlist;
	int			i;

	/*
	 * Find all the pg_amop entries containing the operator.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	op_tuple = &catlist->members[i]->tuple;
		Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);
		StrategyNumber op_strategy;

		/* must be btree */
		if (op_form->amopmethod != BTREE_AM_OID)
			continue;

		/* Get the operator's btree strategy number */
		op_strategy = (StrategyNumber) op_form->amopstrategy;
		Assert(op_strategy >= 1 && op_strategy <= 5);

		thisresult = (OpBtreeInterpretation *)
			palloc(sizeof(OpBtreeInterpretation));
		thisresult->opfamily_id = op_form->amopfamily;
		thisresult->strategy = op_strategy;
		thisresult->oplefttype = op_form->amoplefttype;
		thisresult->oprighttype = op_form->amoprighttype;
		result = lappend(result, thisresult);
	}

	ReleaseSysCacheList(catlist);

	/*
	 * If we didn't find any btree opfamily containing the operator, perhaps
	 * it is a <> operator.  See if it has a negator that is in an opfamily.
	 */
	if (result == NIL)
	{
		Oid			op_negator = get_negator(opno);

		if (OidIsValid(op_negator))
		{
			catlist = SearchSysCacheList1(AMOPOPID,
										  ObjectIdGetDatum(op_negator));

			for (i = 0; i < catlist->n_members; i++)
			{
				HeapTuple	op_tuple = &catlist->members[i]->tuple;
				Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);
				StrategyNumber op_strategy;

				/* must be btree */
				if (op_form->amopmethod != BTREE_AM_OID)
					continue;

				/* Get the operator's btree strategy number */
				op_strategy = (StrategyNumber) op_form->amopstrategy;
				Assert(op_strategy >= 1 && op_strategy <= 5);

				/* Only consider negators that are = */
				if (op_strategy != BTEqualStrategyNumber)
					continue;

				/* OK, report it with "strategy" ROWCOMPARE_NE */
				thisresult = (OpBtreeInterpretation *)
					palloc(sizeof(OpBtreeInterpretation));
				thisresult->opfamily_id = op_form->amopfamily;
				thisresult->strategy = ROWCOMPARE_NE;
				thisresult->oplefttype = op_form->amoplefttype;
				thisresult->oprighttype = op_form->amoprighttype;
				result = lappend(result, thisresult);
			}

			ReleaseSysCacheList(catlist);
		}
	}

	return result;
}

/*
 * equality_ops_are_compatible
 *		Return TRUE if the two given equality operators have compatible
 *		semantics.
 *
 * This is trivially true if they are the same operator.  Otherwise,
 * we look to see if they can be found in the same btree or hash opfamily.
 * Either finding allows us to assume that they have compatible notions
 * of equality.  (The reason we need to do these pushups is that one might
 * be a cross-type operator; for instance int24eq vs int4eq.)
 */
bool
equality_ops_are_compatible(Oid opno1, Oid opno2)
{
	bool		result;
	CatCList   *catlist;
	int			i;

	/* Easy if they're the same operator */
	if (opno1 == opno2)
		return true;

	/*
	 * We search through all the pg_amop entries for opno1.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno1));

	result = false;
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	op_tuple = &catlist->members[i]->tuple;
		Form_pg_amop op_form = (Form_pg_amop) GETSTRUCT(op_tuple);

		/* must be btree or hash */
		if (op_form->amopmethod == BTREE_AM_OID ||
			op_form->amopmethod == HASH_AM_OID)
		{
			if (op_in_opfamily(opno2, op_form->amopfamily))
			{
				result = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}


/*				---------- AMPROC CACHES ----------						 */

/*
 * get_opfamily_proc
 *		Get the OID of the specified support function
 *		for the specified opfamily and datatypes.
 *
 * Returns InvalidOid if there is no pg_amproc entry for the given keys.
 */
Oid
get_opfamily_proc(Oid opfamily, Oid lefttype, Oid righttype, int16 procnum)
{
	HeapTuple	tp;
	Form_pg_amproc amproc_tup;
	RegProcedure result;

	tp = SearchSysCache4(AMPROCNUM,
						 ObjectIdGetDatum(opfamily),
						 ObjectIdGetDatum(lefttype),
						 ObjectIdGetDatum(righttype),
						 Int16GetDatum(procnum));
	if (!HeapTupleIsValid(tp))
		return InvalidOid;
	amproc_tup = (Form_pg_amproc) GETSTRUCT(tp);
	result = amproc_tup->amproc;
	ReleaseSysCache(tp);
	return result;
}


/*				---------- ATTRIBUTE CACHES ----------					 */

/*
 * get_attname
 *		Given the relation id and the attribute number,
 *		return the "attname" field from the attribute relation.
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such attribute.
 */
char *
get_attname(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(att_tup->attname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_relid_attribute_name
 *
 * Same as above routine get_attname(), except that error
 * is handled by elog() instead of returning NULL.
 */
char *
get_relid_attribute_name(Oid relid, AttrNumber attnum)
{
	char	   *attname;

	attname = get_attname(relid, attnum);
	if (attname == NULL)
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	return attname;
}

/*
 * get_attnum
 *
 *		Given the relation id and the attribute name,
 *		return the "attnum" field from the attribute relation.
 *
 *		Returns InvalidAttrNumber if the attr doesn't exist (or is dropped).
 */
AttrNumber
get_attnum(Oid relid, const char *attname)
{
	HeapTuple	tp;

	tp = SearchSysCacheAttName(relid, attname);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		AttrNumber	result;

		result = att_tup->attnum;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}

/*
 * get_atttype
 *
 *		Given the relation OID and the attribute number with the relation,
 *		return the attribute type OID.
 */
Oid
get_atttype(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		Oid			result;

		result = att_tup->atttypid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_atttypmod
 *
 *		Given the relation id and the attribute number,
 *		return the "atttypmod" field from the attribute relation.
 */
int32
get_atttypmod(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		int32		result;

		result = att_tup->atttypmod;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return -1;
}

/*
 * get_atttypetypmodcoll
 *
 *		A three-fer: given the relation id and the attribute number,
 *		fetch atttypid, atttypmod, and attcollation in a single cache lookup.
 *
 * Unlike the otherwise-similar get_atttype/get_atttypmod, this routine
 * raises an error if it can't obtain the information.
 */
void
get_atttypetypmodcoll(Oid relid, AttrNumber attnum,
					  Oid *typid, int32 *typmod, Oid *collid)
{
	HeapTuple	tp;
	Form_pg_attribute att_tup;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	att_tup = (Form_pg_attribute) GETSTRUCT(tp);

	*typid = att_tup->atttypid;
	*typmod = att_tup->atttypmod;
	*collid = att_tup->attcollation;
	ReleaseSysCache(tp);
}

/*				---------- COLLATION CACHE ----------					 */

/*
 * get_collation_name
 *		Returns the name of a given pg_collation entry.
 *
 * Returns a palloc'd copy of the string, or NULL if no such constraint.
 *
 * NOTE: since collation name is not unique, be wary of code that uses this
 * for anything except preparing error messages.
 */
char *
get_collation_name(Oid colloid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(colloid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_collation colltup = (Form_pg_collation) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(colltup->collname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*				---------- CONSTRAINT CACHE ----------					 */

/*
 * get_constraint_name
 *		Returns the name of a given pg_constraint entry.
 *
 * Returns a palloc'd copy of the string, or NULL if no such constraint.
 *
 * NOTE: since constraint name is not unique, be wary of code that uses this
 * for anything except preparing error messages.
 */
char *
get_constraint_name(Oid conoid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(contup->conname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*				---------- OPCLASS CACHE ----------						 */

/*
 * get_opclass_family
 *
 *		Returns the OID of the operator family the opclass belongs to.
 */
Oid
get_opclass_family(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	Oid			result;

	tp = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = cla_tup->opcfamily;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_opclass_input_type
 *
 *		Returns the OID of the datatype the opclass indexes.
 */
Oid
get_opclass_input_type(Oid opclass)
{
	HeapTuple	tp;
	Form_pg_opclass cla_tup;
	Oid			result;

	tp = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	cla_tup = (Form_pg_opclass) GETSTRUCT(tp);

	result = cla_tup->opcintype;
	ReleaseSysCache(tp);
	return result;
}

/*				---------- OPERATOR CACHE ----------					 */

/*
 * get_opcode
 *
 *		Returns the regproc id of the routine used to implement an
 *		operator given the operator oid.
 */
RegProcedure
get_opcode(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprcode;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_opname
 *	  returns the name of the operator with the given opno
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such operator.
 */
char *
get_opname(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(optup->oprname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * op_input_types
 *
 *		Returns the left and right input datatypes for an operator
 *		(InvalidOid if not relevant).
 */
void
op_input_types(Oid opno, Oid *lefttype, Oid *righttype)
{
	HeapTuple	tp;
	Form_pg_operator optup;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tp))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for operator %u", opno);
	optup = (Form_pg_operator) GETSTRUCT(tp);
	*lefttype = optup->oprleft;
	*righttype = optup->oprright;
	ReleaseSysCache(tp);
}

/*
 * op_mergejoinable
 *
 * Returns true if the operator is potentially mergejoinable.  (The planner
 * will fail to find any mergejoin plans unless there are suitable btree
 * opfamily entries for this operator and associated sortops.  The pg_operator
 * flag is just a hint to tell the planner whether to bother looking.)
 *
 * In some cases (currently only array_eq and record_eq), mergejoinability
 * depends on the specific input data type the operator is invoked for, so
 * that must be passed as well. We currently assume that only one input's type
 * is needed to check this --- by convention, pass the left input's data type.
 */
bool
op_mergejoinable(Oid opno, Oid inputtype)
{
	bool		result = false;
	HeapTuple	tp;
	TypeCacheEntry *typentry;

	/*
	 * For array_eq or record_eq, we can sort if the element or field types
	 * are all sortable.  We could implement all the checks for that here, but
	 * the typcache already does that and caches the results too, so let's
	 * rely on the typcache.
	 */
	if (opno == ARRAY_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_CMP_PROC);
		if (typentry->cmp_proc == F_BTARRAYCMP)
			result = true;
	}
	else if (opno == RECORD_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_CMP_PROC);
		if (typentry->cmp_proc == F_BTRECORDCMP)
			result = true;
	}
	else
	{
		/* For all other operators, rely on pg_operator.oprcanmerge */
		tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
		if (HeapTupleIsValid(tp))
		{
			Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

			result = optup->oprcanmerge;
			ReleaseSysCache(tp);
		}
	}
	return result;
}

/*
 * op_hashjoinable
 *
 * Returns true if the operator is hashjoinable.  (There must be a suitable
 * hash opfamily entry for this operator if it is so marked.)
 *
 * In some cases (currently only array_eq), hashjoinability depends on the
 * specific input data type the operator is invoked for, so that must be
 * passed as well.	We currently assume that only one input's type is needed
 * to check this --- by convention, pass the left input's data type.
 */
bool
op_hashjoinable(Oid opno, Oid inputtype)
{
	bool		result = false;
	HeapTuple	tp;
	TypeCacheEntry *typentry;

	/* As in op_mergejoinable, let the typcache handle the hard cases */
	/* Eventually we'll need a similar case for record_eq ... */
	if (opno == ARRAY_EQ_OP)
	{
		typentry = lookup_type_cache(inputtype, TYPECACHE_HASH_PROC);
		if (typentry->hash_proc == F_HASH_ARRAY)
			result = true;
	}
	else
	{
		/* For all other operators, rely on pg_operator.oprcanhash */
		tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
		if (HeapTupleIsValid(tp))
		{
			Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

			result = optup->oprcanhash;
			ReleaseSysCache(tp);
		}
	}
	return result;
}

/*
 * op_strict
 *
 * Get the proisstrict flag for the operator's underlying function.
 */
bool
op_strict(Oid opno)
{
	RegProcedure funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "operator %u does not exist", opno);

	return func_strict((Oid) funcid);
}

/*
 * op_volatile
 *
 * Get the provolatile flag for the operator's underlying function.
 */
char
op_volatile(Oid opno)
{
	RegProcedure funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "operator %u does not exist", opno);

	return func_volatile((Oid) funcid);
}

/*
 * get_commutator
 *
 *		Returns the corresponding commutator of an operator.
 */
Oid
get_commutator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprcom;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_negator
 *
 *		Returns the corresponding negator of an operator.
 */
Oid
get_negator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid			result;

		result = optup->oprnegate;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_oprrest
 *
 *		Returns procedure id for computing selectivity of an operator.
 */
RegProcedure
get_oprrest(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprrest;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_oprjoin
 *
 *		Returns procedure id for computing selectivity of a join.
 */
RegProcedure
get_oprjoin(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure result;

		result = optup->oprjoin;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*				---------- FUNCTION CACHE ----------					 */

/*
 * get_func_name
 *	  returns the name of the function with the given funcid
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such function.
 */
char *
get_func_name(Oid funcid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_proc functup = (Form_pg_proc) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(functup->proname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_func_namespace
 *
 *		Returns the pg_namespace OID associated with a given function.
 */
Oid
get_func_namespace(Oid funcid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_proc functup = (Form_pg_proc) GETSTRUCT(tp);
		Oid			result;

		result = functup->pronamespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_func_rettype
 *		Given procedure id, return the function's result type.
 */
Oid
get_func_rettype(Oid funcid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->prorettype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_nargs
 *		Given procedure id, return the number of arguments.
 */
int
get_func_nargs(Oid funcid)
{
	HeapTuple	tp;
	int			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->pronargs;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_signature
 *		Given procedure id, return the function's argument and result types.
 *		(The return value is the result type.)
 *
 * The arguments are returned as a palloc'd array.
 */
Oid
get_func_signature(Oid funcid, Oid **argtypes, int *nargs)
{
	HeapTuple	tp;
	Form_pg_proc procstruct;
	Oid			result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	procstruct = (Form_pg_proc) GETSTRUCT(tp);

	result = procstruct->prorettype;
	*nargs = (int) procstruct->pronargs;
	Assert(*nargs == procstruct->proargtypes.dim1);
	*argtypes = (Oid *) palloc(*nargs * sizeof(Oid));
	memcpy(*argtypes, procstruct->proargtypes.values, *nargs * sizeof(Oid));

	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_retset
 *		Given procedure id, return the function's proretset flag.
 */
bool
get_func_retset(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proretset;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_strict
 *		Given procedure id, return the function's proisstrict flag.
 */
bool
func_strict(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proisstrict;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_volatile
 *		Given procedure id, return the function's provolatile flag.
 */
char
func_volatile(Oid funcid)
{
	HeapTuple	tp;
	char		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->provolatile;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_leakproof
 *	   Given procedure id, return the function's leakproof field.
 */
bool
get_func_leakproof(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proleakproof;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_cost
 *		Given procedure id, return the function's procost field.
 */
float4
get_func_cost(Oid funcid)
{
	HeapTuple	tp;
	float4		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->procost;
	ReleaseSysCache(tp);
	return result;
}

/*
 * get_func_rows
 *		Given procedure id, return the function's prorows field.
 */
float4
get_func_rows(Oid funcid)
{
	HeapTuple	tp;
	float4		result;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->prorows;
	ReleaseSysCache(tp);
	return result;
}

/*				---------- RELATION CACHE ----------					 */

/*
 * get_relname_relid
 *		Given name and namespace of a relation, look up the OID.
 *
 * Returns InvalidOid if there is no such relation.
 */
Oid
get_relname_relid(const char *relname, Oid relnamespace)
{
	return GetSysCacheOid2(RELNAMENSP,
						   PointerGetDatum(relname),
						   ObjectIdGetDatum(relnamespace));
}

#ifdef NOT_USED
/*
 * get_relnatts
 *
 *		Returns the number of attributes for a given relation.
 */
int
get_relnatts(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		int			result;

		result = reltup->relnatts;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}
#endif

/*
 * get_rel_name
 *		Returns the name of a given relation.
 *
 * Returns a palloc'd copy of the string, or NULL if no such relation.
 *
 * NOTE: since relation name is not unique, be wary of code that uses this
 * for anything except preparing error messages.
 */
char *
get_rel_name(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(reltup->relname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_rel_namespace
 *
 *		Returns the pg_namespace OID associated with a given relation.
 */
Oid
get_rel_namespace(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->relnamespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_type_id
 *
 *		Returns the pg_type OID associated with a given relation.
 *
 * Note: not all pg_class entries have associated pg_type OIDs; so be
 * careful to check for InvalidOid result.
 */
Oid
get_rel_type_id(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->reltype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_rel_relkind
 *
 *		Returns the relkind associated with a given relation.
 */
char
get_rel_relkind(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char		result;

		result = reltup->relkind;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * get_rel_tablespace
 *
 *		Returns the pg_tablespace OID associated with a given relation.
 *
 * Note: InvalidOid might mean either that we couldn't find the relation,
 * or that it is in the database's default tablespace.
 */
Oid
get_rel_tablespace(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		Oid			result;

		result = reltup->reltablespace;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}


/*				---------- TYPE CACHE ----------						 */

/*
 * get_typisdefined
 *
 *		Given the type OID, determine whether the type is defined
 *		(if not, it's only a shell).
 */
bool
get_typisdefined(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool		result;

		result = typtup->typisdefined;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlen
 *
 *		Given the type OID, return the length of the type.
 */
int16
get_typlen(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		int16		result;

		result = typtup->typlen;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 0;
}

/*
 * get_typbyval
 *
 *		Given the type OID, determine whether the type is returned by value or
 *		not.  Returns true if by value, false if by reference.
 */
bool
get_typbyval(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool		result;

		result = typtup->typbyval;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlenbyval
 *
 *		A two-fer: given the type OID, return both typlen and typbyval.
 *
 *		Since both pieces of info are needed to know how to copy a Datum,
 *		many places need both.	Might as well get them with one cache lookup
 *		instead of two.  Also, this routine raises an error instead of
 *		returning a bogus value when given a bad type OID.
 */
void
get_typlenbyval(Oid typid, int16 *typlen, bool *typbyval)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	ReleaseSysCache(tp);
}

/*
 * get_typlenbyvalalign
 *
 *		A three-fer: given the type OID, return typlen, typbyval, typalign.
 */
void
get_typlenbyvalalign(Oid typid, int16 *typlen, bool *typbyval,
					 char *typalign)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	*typalign = typtup->typalign;
	ReleaseSysCache(tp);
}

/*
 * getTypeIOParam
 *		Given a pg_type row, select the type OID to pass to I/O functions
 *
 * Formerly, all I/O functions were passed pg_type.typelem as their second
 * parameter, but we now have a more complex rule about what to pass.
 * This knowledge is intended to be centralized here --- direct references
 * to typelem elsewhere in the code are wrong, if they are associated with
 * I/O calls and not with actual subscripting operations!  (But see
 * bootstrap.c's boot_get_type_io_data() if you need to change this.)
 *
 * As of PostgreSQL 8.1, output functions receive only the value itself
 * and not any auxiliary parameters, so the name of this routine is now
 * a bit of a misnomer ... it should be getTypeInputParam.
 */
Oid
getTypeIOParam(HeapTuple typeTuple)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * Array types get their typelem as parameter; everybody else gets their
	 * own type OID as parameter.
	 */
	if (OidIsValid(typeStruct->typelem))
		return typeStruct->typelem;
	else
		return HeapTupleGetOid(typeTuple);
}

/*
 * get_type_io_data
 *
 *		A six-fer:	given the type OID, return typlen, typbyval, typalign,
 *					typdelim, typioparam, and IO function OID. The IO function
 *					returned is controlled by IOFuncSelector
 */
void
get_type_io_data(Oid typid,
				 IOFuncSelector which_func,
				 int16 *typlen,
				 bool *typbyval,
				 char *typalign,
				 char *typdelim,
				 Oid *typioparam,
				 Oid *func)
{
	HeapTuple	typeTuple;
	Form_pg_type typeStruct;

	/*
	 * In bootstrap mode, pass it off to bootstrap.c.  This hack allows us to
	 * use array_in and array_out during bootstrap.
	 */
	if (IsBootstrapProcessingMode())
	{
		Oid			typinput;
		Oid			typoutput;

		boot_get_type_io_data(typid,
							  typlen,
							  typbyval,
							  typalign,
							  typdelim,
							  typioparam,
							  &typinput,
							  &typoutput);
		switch (which_func)
		{
			case IOFunc_input:
				*func = typinput;
				break;
			case IOFunc_output:
				*func = typoutput;
				break;
			default:
				elog(ERROR, "binary I/O not supported during bootstrap");
				break;
		}
		return;
	}

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	*typlen = typeStruct->typlen;
	*typbyval = typeStruct->typbyval;
	*typalign = typeStruct->typalign;
	*typdelim = typeStruct->typdelim;
	*typioparam = getTypeIOParam(typeTuple);
	switch (which_func)
	{
		case IOFunc_input:
			*func = typeStruct->typinput;
			break;
		case IOFunc_output:
			*func = typeStruct->typoutput;
			break;
		case IOFunc_receive:
			*func = typeStruct->typreceive;
			break;
		case IOFunc_send:
			*func = typeStruct->typsend;
			break;
	}
	ReleaseSysCache(typeTuple);
}

#ifdef NOT_USED
char
get_typalign(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typalign;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 'i';
}
#endif

char
get_typstorage(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typstorage;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 'p';
}

/*
 * get_typdefault
 *	  Given a type OID, return the type's default value, if any.
 *
 *	  The result is a palloc'd expression node tree, or NULL if there
 *	  is no defined default for the datatype.
 *
 * NB: caller should be prepared to coerce result to correct datatype;
 * the returned expression tree might produce something of the wrong type.
 */
Node *
get_typdefault(Oid typid)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	Datum		datum;
	bool		isNull;
	Node	   *expr;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", typid);
	type = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * typdefault and typdefaultbin are potentially null, so don't try to
	 * access 'em as struct fields. Must do it the hard way with
	 * SysCacheGetAttr.
	 */
	datum = SysCacheGetAttr(TYPEOID,
							typeTuple,
							Anum_pg_type_typdefaultbin,
							&isNull);

	if (!isNull)
	{
		/* We have an expression default */
		expr = stringToNode(TextDatumGetCString(datum));
	}
	else
	{
		/* Perhaps we have a plain literal default */
		datum = SysCacheGetAttr(TYPEOID,
								typeTuple,
								Anum_pg_type_typdefault,
								&isNull);

		if (!isNull)
		{
			char	   *strDefaultVal;

			/* Convert text datum to C string */
			strDefaultVal = TextDatumGetCString(datum);
			/* Convert C string to a value of the given type */
			datum = OidInputFunctionCall(type->typinput, strDefaultVal,
										 getTypeIOParam(typeTuple), -1);
			/* Build a Const node containing the value */
			expr = (Node *) makeConst(typid,
									  -1,
									  type->typcollation,
									  type->typlen,
									  datum,
									  false,
									  type->typbyval);
			pfree(strDefaultVal);
		}
		else
		{
			/* No default */
			expr = NULL;
		}
	}

	ReleaseSysCache(typeTuple);

	return expr;
}

/*
 * getBaseType
 *		If the given type is a domain, return its base type;
 *		otherwise return the type's own OID.
 */
Oid
getBaseType(Oid typid)
{
	int32		typmod = -1;

	return getBaseTypeAndTypmod(typid, &typmod);
}

/*
 * getBaseTypeAndTypmod
 *		If the given type is a domain, return its base type and typmod;
 *		otherwise return the type's own OID, and leave *typmod unchanged.
 *
 * Note that the "applied typmod" should be -1 for every domain level
 * above the bottommost; therefore, if the passed-in typid is indeed
 * a domain, *typmod should be -1.
 */
Oid
getBaseTypeAndTypmod(Oid typid, int32 *typmod)
{
	/*
	 * We loop to find the bottom base type in a stack of domains.
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typid);
		typTup = (Form_pg_type) GETSTRUCT(tup);
		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* Not a domain, so done */
			ReleaseSysCache(tup);
			break;
		}

		Assert(*typmod == -1);
		typid = typTup->typbasetype;
		*typmod = typTup->typtypmod;

		ReleaseSysCache(tup);
	}

	return typid;
}

/*
 * get_typavgwidth
 *
 *	  Given a type OID and a typmod value (pass -1 if typmod is unknown),
 *	  estimate the average width of values of the type.  This is used by
 *	  the planner, which doesn't require absolutely correct results;
 *	  it's OK (and expected) to guess if we don't know for sure.
 */
int32
get_typavgwidth(Oid typid, int32 typmod)
{
	int			typlen = get_typlen(typid);
	int32		maxwidth;

	/*
	 * Easy if it's a fixed-width type
	 */
	if (typlen > 0)
		return typlen;

	/*
	 * type_maximum_size knows the encoding of typmod for some datatypes;
	 * don't duplicate that knowledge here.
	 */
	maxwidth = type_maximum_size(typid, typmod);
	if (maxwidth > 0)
	{
		/*
		 * For BPCHAR, the max width is also the only width.  Otherwise we
		 * need to guess about the typical data width given the max. A sliding
		 * scale for percentage of max width seems reasonable.
		 */
		if (typid == BPCHAROID)
			return maxwidth;
		if (maxwidth <= 32)
			return maxwidth;	/* assume full width */
		if (maxwidth < 1000)
			return 32 + (maxwidth - 32) / 2;	/* assume 50% */

		/*
		 * Beyond 1000, assume we're looking at something like
		 * "varchar(10000)" where the limit isn't actually reached often, and
		 * use a fixed estimate.
		 */
		return 32 + (1000 - 32) / 2;
	}

	/*
	 * Ooops, we have no idea ... wild guess time.
	 */
	return 32;
}

/*
 * get_typtype
 *
 *		Given the type OID, find if it is a basic type, a complex type, etc.
 *		It returns the null char if the cache lookup fails...
 */
char
get_typtype(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char		result;

		result = typtup->typtype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

/*
 * type_is_rowtype
 *
 *		Convenience function to determine whether a type OID represents
 *		a "rowtype" type --- either RECORD or a named composite type.
 */
bool
type_is_rowtype(Oid typid)
{
	return (typid == RECORDOID || get_typtype(typid) == TYPTYPE_COMPOSITE);
}

/*
 * type_is_enum
 *	  Returns true if the given type is an enum type.
 */
bool
type_is_enum(Oid typid)
{
	return (get_typtype(typid) == TYPTYPE_ENUM);
}

/*
 * type_is_range
 *	  Returns true if the given type is a range type.
 */
bool
type_is_range(Oid typid)
{
	return (get_typtype(typid) == TYPTYPE_RANGE);
}

/*
 * get_type_category_preferred
 *
 *		Given the type OID, fetch its category and preferred-type status.
 *		Throws error on failure.
 */
void
get_type_category_preferred(Oid typid, char *typcategory, bool *typispreferred)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typcategory = typtup->typcategory;
	*typispreferred = typtup->typispreferred;
	ReleaseSysCache(tp);
}

/*
 * get_typ_typrelid
 *
 *		Given the type OID, get the typrelid (InvalidOid if not a complex
 *		type).
 */
Oid
get_typ_typrelid(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typrelid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_element_type
 *
 *		Given the type OID, get the typelem (InvalidOid if not an array type).
 *
 * NB: this only considers varlena arrays to be true arrays; InvalidOid is
 * returned if the input is a fixed-length array type.
 */
Oid
get_element_type(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		if (typtup->typlen == -1)
			result = typtup->typelem;
		else
			result = InvalidOid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_array_type
 *
 *		Given the type OID, get the corresponding "true" array type.
 *		Returns InvalidOid if no array type can be found.
 */
Oid
get_array_type(Oid typid)
{
	HeapTuple	tp;
	Oid			result = InvalidOid;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_type) GETSTRUCT(tp))->typarray;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * get_base_element_type
 *		Given the type OID, get the typelem, looking "through" any domain
 *		to its underlying array type.
 *
 * This is equivalent to get_element_type(getBaseType(typid)), but avoids
 * an extra cache lookup.  Note that it fails to provide any information
 * about the typmod of the array.
 */
Oid
get_base_element_type(Oid typid)
{
	/*
	 * We loop to find the bottom base type in a stack of domains.
	 */
	for (;;)
	{
		HeapTuple	tup;
		Form_pg_type typTup;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			break;
		typTup = (Form_pg_type) GETSTRUCT(tup);
		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* Not a domain, so stop descending */
			Oid			result;

			/* This test must match get_element_type */
			if (typTup->typlen == -1)
				result = typTup->typelem;
			else
				result = InvalidOid;
			ReleaseSysCache(tup);
			return result;
		}

		typid = typTup->typbasetype;
		ReleaseSysCache(tup);
	}

	/* Like get_element_type, silently return InvalidOid for bogus input */
	return InvalidOid;
}

/*
 * getTypeInputInfo
 *
 *		Get info needed for converting values of a type to internal form
 */
void
getTypeInputInfo(Oid type, Oid *typInput, Oid *typIOParam)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typinput))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no input function available for type %s",
						format_type_be(type))));

	*typInput = pt->typinput;
	*typIOParam = getTypeIOParam(typeTuple);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeOutputInfo
 *
 *		Get info needed for printing values of a type
 */
void
getTypeOutputInfo(Oid type, Oid *typOutput, bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typoutput))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no output function available for type %s",
						format_type_be(type))));

	*typOutput = pt->typoutput;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeBinaryInputInfo
 *
 *		Get info needed for binary input of values of a type
 */
void
getTypeBinaryInputInfo(Oid type, Oid *typReceive, Oid *typIOParam)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typreceive))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no binary input function available for type %s",
						format_type_be(type))));

	*typReceive = pt->typreceive;
	*typIOParam = getTypeIOParam(typeTuple);

	ReleaseSysCache(typeTuple);
}

/*
 * getTypeBinaryOutputInfo
 *
 *		Get info needed for binary output of values of a type
 */
void
getTypeBinaryOutputInfo(Oid type, Oid *typSend, bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "cache lookup failed for type %u", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	if (!pt->typisdefined)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type %s is only a shell",
						format_type_be(type))));
	if (!OidIsValid(pt->typsend))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("no binary output function available for type %s",
						format_type_be(type))));

	*typSend = pt->typsend;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);

	ReleaseSysCache(typeTuple);
}

/*
 * get_typmodin
 *
 *		Given the type OID, return the type's typmodin procedure, if any.
 */
Oid
get_typmodin(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typmodin;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

#ifdef NOT_USED
/*
 * get_typmodout
 *
 *		Given the type OID, return the type's typmodout procedure, if any.
 */
Oid
get_typmodout(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typmodout;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}
#endif   /* NOT_USED */

/*
 * get_typcollation
 *
 *		Given the type OID, return the type's typcollation attribute.
 */
Oid
get_typcollation(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		Oid			result;

		result = typtup->typcollation;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}


/*
 * type_is_collatable
 *
 *		Return whether the type cares about collations
 */
bool
type_is_collatable(Oid typid)
{
	return OidIsValid(get_typcollation(typid));
}


/*				---------- STATISTICS CACHE ----------					 */

/*
 * get_attavgwidth
 *
 *	  Given the table and attribute number of a column, get the average
 *	  width of entries in the column.  Return zero if no data available.
 *
 * Currently this is only consulted for individual tables, not for inheritance
 * trees, so we don't need an "inh" parameter.
 *
 * Calling a hook at this point looks somewhat strange, but is required
 * because the optimizer calls this function without any other way for
 * plug-ins to control the result.
 */
int32
get_attavgwidth(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;
	int32		stawidth;

	if (get_attavgwidth_hook)
	{
		stawidth = (*get_attavgwidth_hook) (relid, attnum);
		if (stawidth > 0)
			return stawidth;
	}
	tp = SearchSysCache3(STATRELATTINH,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum),
						 BoolGetDatum(false));
	if (HeapTupleIsValid(tp))
	{
		stawidth = ((Form_pg_statistic) GETSTRUCT(tp))->stawidth;
		ReleaseSysCache(tp);
		if (stawidth > 0)
			return stawidth;
	}
	return 0;
}

/*
 * get_attstatsslot
 *
 *		Extract the contents of a "slot" of a pg_statistic tuple.
 *		Returns TRUE if requested slot type was found, else FALSE.
 *
 * Unlike other routines in this file, this takes a pointer to an
 * already-looked-up tuple in the pg_statistic cache.  We do this since
 * most callers will want to extract more than one value from the cache
 * entry, and we don't want to repeat the cache lookup unnecessarily.
 * Also, this API allows this routine to be used with statistics tuples
 * that have been provided by a stats hook and didn't really come from
 * pg_statistic.
 *
 * statstuple: pg_statistics tuple to be examined.
 * atttype: type OID of attribute (can be InvalidOid if values == NULL).
 * atttypmod: typmod of attribute (can be 0 if values == NULL).
 * reqkind: STAKIND code for desired statistics slot kind.
 * reqop: STAOP value wanted, or InvalidOid if don't care.
 * actualop: if not NULL, *actualop receives the actual STAOP value.
 * values, nvalues: if not NULL, the slot's stavalues are extracted.
 * numbers, nnumbers: if not NULL, the slot's stanumbers are extracted.
 *
 * If assigned, values and numbers are set to point to palloc'd arrays.
 * If the attribute type is pass-by-reference, the values referenced by
 * the values array are themselves palloc'd.  The palloc'd stuff can be
 * freed by calling free_attstatsslot.
 *
 * Note: at present, atttype/atttypmod aren't actually used here at all.
 * But the caller must have the correct (or at least binary-compatible)
 * type ID to pass to free_attstatsslot later.
 */
bool
get_attstatsslot(HeapTuple statstuple,
				 Oid atttype, int32 atttypmod,
				 int reqkind, Oid reqop,
				 Oid *actualop,
				 Datum **values, int *nvalues,
				 float4 **numbers, int *nnumbers)
{
	Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(statstuple);
	int			i,
				j;
	Datum		val;
	bool		isnull;
	ArrayType  *statarray;
	Oid			arrayelemtype;
	int			narrayelem;
	HeapTuple	typeTuple;
	Form_pg_type typeForm;

	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		if ((&stats->stakind1)[i] == reqkind &&
			(reqop == InvalidOid || (&stats->staop1)[i] == reqop))
			break;
	}
	if (i >= STATISTIC_NUM_SLOTS)
		return false;			/* not there */

	if (actualop)
		*actualop = (&stats->staop1)[i];

	if (values)
	{
		val = SysCacheGetAttr(STATRELATTINH, statstuple,
							  Anum_pg_statistic_stavalues1 + i,
							  &isnull);
		if (isnull)
			elog(ERROR, "stavalues is null");
		statarray = DatumGetArrayTypeP(val);

		/*
		 * Need to get info about the array element type.  We look at the
		 * actual element type embedded in the array, which might be only
		 * binary-compatible with the passed-in atttype.  The info we extract
		 * here should be the same either way, but deconstruct_array is picky
		 * about having an exact type OID match.
		 */
		arrayelemtype = ARR_ELEMTYPE(statarray);
		typeTuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(arrayelemtype));
		if (!HeapTupleIsValid(typeTuple))
			elog(ERROR, "cache lookup failed for type %u", arrayelemtype);
		typeForm = (Form_pg_type) GETSTRUCT(typeTuple);

		/* Deconstruct array into Datum elements; NULLs not expected */
		deconstruct_array(statarray,
						  arrayelemtype,
						  typeForm->typlen,
						  typeForm->typbyval,
						  typeForm->typalign,
						  values, NULL, nvalues);

		/*
		 * If the element type is pass-by-reference, we now have a bunch of
		 * Datums that are pointers into the syscache value.  Copy them to
		 * avoid problems if syscache decides to drop the entry.
		 */
		if (!typeForm->typbyval)
		{
			for (j = 0; j < *nvalues; j++)
			{
				(*values)[j] = datumCopy((*values)[j],
										 typeForm->typbyval,
										 typeForm->typlen);
			}
		}

		ReleaseSysCache(typeTuple);

		/*
		 * Free statarray if it's a detoasted copy.
		 */
		if ((Pointer) statarray != DatumGetPointer(val))
			pfree(statarray);
	}

	if (numbers)
	{
		val = SysCacheGetAttr(STATRELATTINH, statstuple,
							  Anum_pg_statistic_stanumbers1 + i,
							  &isnull);
		if (isnull)
			elog(ERROR, "stanumbers is null");
		statarray = DatumGetArrayTypeP(val);

		/*
		 * We expect the array to be a 1-D float4 array; verify that. We don't
		 * need to use deconstruct_array() since the array data is just going
		 * to look like a C array of float4 values.
		 */
		narrayelem = ARR_DIMS(statarray)[0];
		if (ARR_NDIM(statarray) != 1 || narrayelem <= 0 ||
			ARR_HASNULL(statarray) ||
			ARR_ELEMTYPE(statarray) != FLOAT4OID)
			elog(ERROR, "stanumbers is not a 1-D float4 array");
		*numbers = (float4 *) palloc(narrayelem * sizeof(float4));
		memcpy(*numbers, ARR_DATA_PTR(statarray), narrayelem * sizeof(float4));
		*nnumbers = narrayelem;

		/*
		 * Free statarray if it's a detoasted copy.
		 */
		if ((Pointer) statarray != DatumGetPointer(val))
			pfree(statarray);
	}

	return true;
}

/*
 * free_attstatsslot
 *		Free data allocated by get_attstatsslot
 *
 * atttype need be valid only if values != NULL.
 */
void
free_attstatsslot(Oid atttype,
				  Datum *values, int nvalues,
				  float4 *numbers, int nnumbers)
{
	if (values)
	{
		if (!get_typbyval(atttype))
		{
			int			i;

			for (i = 0; i < nvalues; i++)
				pfree(DatumGetPointer(values[i]));
		}
		pfree(values);
	}
	if (numbers)
		pfree(numbers);
}

/*				---------- PG_NAMESPACE CACHE ----------				 */

/*
 * get_namespace_name
 *		Returns the name of a given namespace
 *
 * Returns a palloc'd copy of the string, or NULL if no such namespace.
 */
char *
get_namespace_name(Oid nspid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(NAMESPACEOID, ObjectIdGetDatum(nspid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_namespace nsptup = (Form_pg_namespace) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(nsptup->nspname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*				---------- PG_RANGE CACHE ----------				 */

/*
 * get_range_subtype
 *		Returns the subtype of a given range type
 *
 * Returns InvalidOid if the type is not a range type.
 */
Oid
get_range_subtype(Oid rangeOid)
{
	HeapTuple	tp;

	tp = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(rangeOid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_range rngtup = (Form_pg_range) GETSTRUCT(tp);
		Oid			result;

		result = rngtup->rngsubtype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}
