/*-------------------------------------------------------------------------
 *
 * clausesel.c--
 *    Routines to compute and set clause selectivities 
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/optimizer/path/clausesel.c,v 1.1.1.1 1996/07/09 06:21:35 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "optimizer/internal.h"
#include "optimizer/clauses.h"
#include "optimizer/clauseinfo.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"

#include "parser/parsetree.h"		/* for getrelid() */

#include "catalog/pg_proc.h"
#include "catalog/pg_operator.h"

#include "utils/elog.h"
#include "utils/lsyscache.h"

static Cost compute_selec(Query *root, List *clauses, List *or_selectivities);

/****************************************************************************
 * 	ROUTINES TO SET CLAUSE SELECTIVITIES  
 ****************************************************************************/

/*    
 * set_clause_selectivities -
 *    Sets the selectivity field for each of clause in 'clauseinfo-list'
 *    to 'new-selectivity'.  If the selectivity has already been set, reset 
 *    it only if the new one is better.
 *    
 * Returns nothing of interest.
 *
 */
void
set_clause_selectivities(List *clauseinfo_list, Cost new_selectivity)
{
    List *temp;
    CInfo *clausenode;
    Cost cost_clause;

    foreach (temp,clauseinfo_list) {
	clausenode = (CInfo*)lfirst(temp);
	cost_clause = clausenode->selectivity;
	if ( FLOAT_IS_ZERO(cost_clause) || new_selectivity < cost_clause) {
	    clausenode->selectivity = new_selectivity;
	}
    }
}

/*    
 * product_selec -
 *    Multiplies the selectivities of each clause in 'clauseinfo-list'.
 *    
 * Returns a flonum corresponding to the selectivity of 'clauseinfo-list'.
 */
Cost
product_selec(List *clauseinfo_list)
{
    Cost result = 1.0;
    if (clauseinfo_list!=NIL) {
	List *xclausenode = NIL;
	Cost temp;

	foreach(xclausenode,clauseinfo_list) {
	    temp = ((CInfo *)lfirst(xclausenode))->selectivity;
	    result = result * temp;
	}
    }
    return(result);
}

/*    
 * set_rest_relselec -
 *    Scans through clauses on each relation and assigns a selectivity to
 *    those clauses that haven't been assigned a selectivity by an index.
 *    
 * Returns nothing of interest.
 * MODIFIES: selectivities of the various rel's clauseinfo
 *	  slots. 
 */
void
set_rest_relselec(Query *root, List *rel_list)
{
    Rel *rel;
    List *x;

    foreach (x,rel_list) {
	rel = (Rel*)lfirst(x);
	set_rest_selec(root, rel->clauseinfo);
    }
}

/*    
 * set_rest_selec -
 *    Sets the selectivity fields for those clauses within a single
 *    relation's 'clauseinfo-list' that haven't already been set.
 *    
 * Returns nothing of interest.
 *    
 */
void
set_rest_selec(Query *root, List *clauseinfo_list)
{
    List *temp = NIL;
    CInfo *clausenode = (CInfo*)NULL;
    Cost cost_clause;
    
    foreach (temp,clauseinfo_list) {
	clausenode = (CInfo*)lfirst(temp);
	cost_clause = clausenode->selectivity;

	/*
	 * Check to see if the selectivity of this clause or any 'or'
	 * subclauses (if any) haven't been set yet.
	 */
	if (valid_or_clause(clausenode) || FLOAT_IS_ZERO(cost_clause)) {
	    clausenode->selectivity =
		compute_clause_selec(root, 
				     (Node*)clausenode->clause,
				     lcons(makeFloat(cost_clause), NIL));
	}
    }
}

/****************************************************************************
 *	ROUTINES TO COMPUTE SELECTIVITIES
 ****************************************************************************/

/*    
 * compute_clause_selec -
 *    Given a clause, this routine will compute the selectivity of the
 *    clause by calling 'compute_selec' with the appropriate parameters
 *    and possibly use that return value to compute the real selectivity
 *    of a clause.
 *    
 * 'or-selectivities' are selectivities that have already been assigned
 * 	to subclauses of an 'or' clause.
 *    
 * Returns a flonum corresponding to the clause selectivity.
 *    
 */
Cost
compute_clause_selec(Query *root, Node *clause, List *or_selectivities)
{
    if (!is_opclause (clause)) {
	/* if it's not an operator clause, then it is a boolean clause -jolly*/
	/*
	 * Boolean variables get a selectivity of 1/2.
	 */
	return(0.1);
    } else if (not_clause (clause)) {
	/*
	 * 'not' gets "1.0 - selectivity-of-inner-clause".
	 */
	return (1.000000 - compute_selec(root,
					 lcons(get_notclausearg((Expr*)clause),
					      NIL),
					 or_selectivities));
    } else if (or_clause(clause)) {
	/*
	 * Both 'or' and 'and' clauses are evaluated as described in 
	 *    (compute_selec). 
	 */
	return (compute_selec(root,
			      ((Expr*)clause)->args, or_selectivities));
    } else {
	return(compute_selec(root,
			     lcons(clause,NIL),or_selectivities));
    } 
}

/*    
 * compute_selec - 
 *    Computes the selectivity of a clause.
 *    
 *    If there is more than one clause in the argument 'clauses', then the
 *    desired selectivity is that of an 'or' clause.  Selectivities for an
 *    'or' clause such as (OR a b) are computed by finding the selectivity
 *    of a (s1) and b (s2) and computing s1+s2 - s1*s2.
 *    
 *    In addition, if the clause is an 'or' clause, individual selectivities
 *    may have already been assigned by indices to subclauses.  These values
 *    are contained in the list 'or-selectivities'.
 *    
 * Returns the clause selectivity as a flonum.
 *    
 */
static Cost
compute_selec(Query *root, List *clauses, List *or_selectivities)
{
    Cost s1 = 0;
    List *clause = lfirst(clauses);

    if (clauses==NULL) {
	s1 = 1.0;
    } else if (IsA(clause,Param)) {
	/* XXX How're we handling this before?? -ay */
	s1 = 1.0;
    } else if (IsA(clause,Const)) {
	s1 = ((bool) ((Const*) clause)->constvalue) ? 1.0 : 0.0;
    } else if (IsA(clause,Var)) {
	Oid relid = getrelid(((Var*)clause)->varno,
			     root->rtable);

	/*
	 * we have a bool Var.  This is exactly equivalent to the clause:
	 *	reln.attribute = 't'
	 * so we compute the selectivity as if that is what we have. The
	 * magic #define constants are a hack.  I didn't want to have to
	 * do system cache look ups to find out all of that info.
	 */

	s1 = restriction_selectivity(EqualSelectivityProcedure,
				     BooleanEqualOperator,
				     relid,
				     ((Var*)clause)->varoattno,
				     "t",
				     _SELEC_CONSTANT_RIGHT_);
    } else if (or_selectivities) {
	/* If s1 has already been assigned by an index, use that value. */ 
	List *this_sel = lfirst(or_selectivities);

	s1 = floatVal(this_sel);
    } else if (is_funcclause((Node*)clause)) {
	/* this isn't an Oper, it's a Func!! */
	/*
	 ** This is not an operator, so we guess at the selectivity.  
	 ** THIS IS A HACK TO GET V4 OUT THE DOOR.  FUNCS SHOULD BE
	 ** ABLE TO HAVE SELECTIVITIES THEMSELVES.
	 **     -- JMH 7/9/92
	 */
	s1 = 0.1;
    } else if (NumRelids((Node*) clause) == 1) {
	/* ...otherwise, calculate s1 from 'clauses'. 
	 *    The clause is not a join clause, since there is 
	 *    only one relid in the clause.  The clause 
	 *    selectivity will be based on the operator 
	 *    selectivity and operand values. 
	 */
	Oid opno = ((Oper*)((Expr*)clause)->oper)->opno;
	RegProcedure oprrest = get_oprrest(opno);
	Oid relid;
	int relidx;
	AttrNumber attno;
	Datum constval;
	int flag;

	get_relattval((Node*)clause, &relidx, &attno, &constval, &flag);
	relid = getrelid(relidx, root->rtable); 
	
	/* if the oprrest procedure is missing for whatever reason,
	   use a selectivity of 0.5*/
	if (!oprrest)
	    s1 = (Cost) (0.5);
	else
	    if (attno == InvalidAttrNumber)  {
		/* attno can be Invalid if the clause had a function in it,
		   i.e.   WHERE myFunc(f) = 10 */
		/* this should be FIXED somehow to use function selectivity */
		s1 = (Cost) (0.5);
	    } else
		s1 = (Cost) restriction_selectivity(oprrest,
						opno,
						relid,
						attno,
						(char *)constval,
						flag);

    } else {
	/*    The clause must be a join clause.  The clause 
	 *    selectivity will be based on the relations to be 
	 *    scanned and the attributes they are to be joined 
	 *    on. 
	 */
	Oid opno = ((Oper*)((Expr*)clause)->oper)->opno;
	RegProcedure oprjoin = get_oprjoin (opno);
	int relid1, relid2;
	AttrNumber attno1, attno2;

	get_rels_atts((Node*)clause, &relid1, &attno1, &relid2, &attno2);
	relid1 = getrelid(relid1, root->rtable);
	relid2 = getrelid(relid2, root->rtable);

	/* if the oprjoin procedure is missing for whatever reason,
	   use a selectivity of 0.5*/
	if (!oprjoin)
	    s1 = (Cost) (0.5);
	else
	    s1 = (Cost) join_selectivity(oprjoin,
					 opno,
					 relid1,
					 attno1,
					 relid2,
					 attno2);
    }
    
    /*    A null clause list eliminates no tuples, so return a selectivity 
     *    of 1.0.  If there is only one clause, the selectivity is not 
     *    that of an 'or' clause, but rather that of the single clause.
     */
    
    if (length (clauses) < 2) {
	return(s1);
    } else {
	/* Compute selectivity of the 'or'ed subclauses. */
	/* Added check for taking lnext(NIL).  -- JMH 3/9/92 */
	Cost s2;

	if (or_selectivities != NIL)
	    s2 = compute_selec(root, lnext(clauses), lnext(or_selectivities));
	else
	    s2 = compute_selec(root, lnext(clauses), NIL);
	return(s1 + s2 - s1 * s2);
    }
}

