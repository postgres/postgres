/*-------------------------------------------------------------------------
 *
 * parse_query.c--
 *	  take an "optimizable" stmt and make the query tree that
 *	   the planner requires.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/Attic/parse_query.c,v 1.22 1997/11/02 15:25:30 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>
#include "postgres.h"

#include "fmgr.h"
#include "access/heapam.h"
#include "utils/tqual.h"
#include "access/tupmacs.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/acl.h"			/* for ACL_NO_PRIV_WARNING */
#include "utils/rel.h"			/* Relation stuff */

#include "utils/syscache.h"
#include "catalog/pg_type.h"
#include "catalog/pg_operator.h"
#include "parser/catalog_utils.h"
#include "parser/parse_query.h"
#include "utils/lsyscache.h"

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"

static void
checkTargetTypes(ParseState *pstate, char *target_colname,
				 char *refname, char *colname);

Oid		   *param_type_info;
int			pfunc_num_args;

/* given refname, return a pointer to the range table entry */
RangeTblEntry *
refnameRangeTableEntry(List *rtable, char *refname)
{
	List	   *temp;

	foreach(temp, rtable)
	{
		RangeTblEntry *rte = lfirst(temp);

		if (!strcmp(rte->refname, refname))
			return rte;
	}
	return NULL;
}

/* given refname, return id of variable; position starts with 1 */
int
refnameRangeTablePosn(List *rtable, char *refname)
{
	int			index;
	List	   *temp;

	index = 1;
	foreach(temp, rtable)
	{
		RangeTblEntry *rte = lfirst(temp);

		if (!strcmp(rte->refname, refname))
			return index;
		index++;
	}
	return (0);
}

/*
 * returns range entry if found, else NULL
 */
RangeTblEntry *
colnameRangeTableEntry(ParseState *pstate, char *colname)
{
	List	   *et;
	List	   *rtable;
	RangeTblEntry *rte_result;

	if (pstate->p_is_rule)
		rtable = lnext(lnext(pstate->p_rtable));
	else
		rtable = pstate->p_rtable;

	rte_result = NULL;
	foreach(et, rtable)
	{
		RangeTblEntry *rte = lfirst(et);

		/* only entries on outer(non-function?) scope */
		if (!rte->inFromCl && rte != pstate->p_target_rangetblentry)
			continue;

		if (get_attnum(rte->relid, colname) != InvalidAttrNumber)
		{
			if (rte_result != NULL)
			{
				if (!pstate->p_is_insert ||
					rte != pstate->p_target_rangetblentry)
					elog(WARN, "Column %s is ambiguous", colname);
			}
			else
				rte_result = rte;
		}
	}
	return rte_result;
}

/*
 * put new entry in pstate p_rtable structure, or return pointer
 * if pstate null
*/
RangeTblEntry *
addRangeTableEntry(ParseState *pstate,
				   char *relname,
				   char *refname,
				   bool inh, bool inFromCl,
				   TimeRange *timeRange)
{
	Relation	relation;
	RangeTblEntry *rte = makeNode(RangeTblEntry);

	if (pstate != NULL &&
		refnameRangeTableEntry(pstate->p_rtable, refname) != NULL)
		elog(WARN, "Table name %s specified more than once", refname);

	rte->relname = pstrdup(relname);
	rte->refname = pstrdup(refname);

	relation = heap_openr(relname);
	if (relation == NULL)
	{
		elog(WARN, "%s: %s",
			 relname, aclcheck_error_strings[ACLCHECK_NO_CLASS]);
	}

	/*
	 * Flags - zero or more from archive,inheritance,union,version or
	 * recursive (transitive closure) [we don't support them all -- ay
	 * 9/94 ]
	 */
	rte->inh = inh;

	rte->timeRange = timeRange;

	/* RelOID */
	rte->relid = RelationGetRelationId(relation);

	rte->archive = false;

	rte->inFromCl = inFromCl;

	/*
	 * close the relation we're done with it for now.
	 */
	if (pstate != NULL)
		pstate->p_rtable = lappend(pstate->p_rtable, rte);

	heap_close(relation);

	return rte;
}

/*
 * expandAll -
 *	  makes a list of attributes
 *	  assumes reldesc caching works
 */
List	   *
expandAll(ParseState *pstate, char *relname, char *refname, int *this_resno)
{
	Relation	rdesc;
	List	   *te_tail = NIL,
			   *te_head = NIL;
	Var		   *varnode;
	int			varattno,
				maxattrs;
	Oid			type_id;
	int			type_len;
	RangeTblEntry *rte;

	rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	if (rte == NULL)
		rte = addRangeTableEntry(pstate, relname, refname, FALSE, FALSE, NULL);

	rdesc = heap_open(rte->relid);

	if (rdesc == NULL)
	{
		elog(WARN, "Unable to expand all -- heap_open failed on %s",
			 rte->refname);
		return NIL;
	}
	maxattrs = RelationGetNumberOfAttributes(rdesc);

	for (varattno = 0; varattno <= maxattrs - 1; varattno++)
	{
		char	   *attrname;
		char	   *resname = NULL;
		TargetEntry *te = makeNode(TargetEntry);

		attrname = pstrdup((rdesc->rd_att->attrs[varattno]->attname).data);
		varnode = (Var *) make_var(pstate, refname, attrname, &type_id);
		type_len = (int) tlen(get_id_type(type_id));

		handleTargetColname(pstate, &resname, refname, attrname);
		if (resname != NULL)
			attrname = resname;

		/*
		 * Even if the elements making up a set are complex, the set
		 * itself is not.
		 */

		te->resdom = makeResdom((AttrNumber) (*this_resno)++,
								type_id,
								(Size) type_len,
								attrname,
								(Index) 0,
								(Oid) 0,
								0);
		te->expr = (Node *) varnode;
		if (te_head == NIL)
			te_head = te_tail = lcons(te, NIL);
		else
			te_tail = lappend(te_tail, te);
	}

	heap_close(rdesc);
	return (te_head);
}

static void
disallow_setop(char *op, Type optype, Node *operand)
{
	if (operand == NULL)
		return;

	if (nodeTag(operand) == T_Iter)
	{
		elog(NOTICE, "An operand to the '%s' operator returns a set of %s,",
			 op, tname(optype));
		elog(WARN, "but '%s' takes single values, not sets.",
			 op);
	}
}

static Node *
make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid true_typeId)
{
	Node	   *result;
	Type		true_type;
	Datum		val;
	Oid			infunc;

	if (tree != NULL)
	{
		result = tree;
		true_type = get_id_type(true_typeId);
		disallow_setop(opname, true_type, result);
		if (true_typeId != orig_typeId)
		{						/* must coerce */
			Const	   *con = (Const *) result;

			Assert(nodeTag(result) == T_Const);
			val = (Datum) textout((struct varlena *)
								  con->constvalue);
			infunc = typeid_get_retinfunc(true_typeId);
			con = makeNode(Const);
			con->consttype = true_typeId;
			con->constlen = tlen(true_type);
			con->constvalue = (Datum) fmgr(infunc,
										   val,
										   get_typelem(true_typeId),
										   -1 /* for varchar() type */ );
			con->constisnull = false;
			con->constbyval = true;
			con->constisset = false;
			result = (Node *) con;
		}
	}
	else
	{
		Const	   *con = makeNode(Const);

		con->consttype = true_typeId;
		con->constlen = 0;
		con->constvalue = (Datum) (struct varlena *) NULL;
		con->constisnull = true;
		con->constbyval = true;
		con->constisset = false;
		result = (Node *) con;
	}

	return result;
}


Expr	   *
make_op(char *opname, Node *ltree, Node *rtree)
{
	Oid			ltypeId,
				rtypeId;
	Operator	temp;
	OperatorTupleForm opform;
	Oper	   *newop;
	Node	   *left,
			   *right;
	Expr	   *result;

	if (rtree == NULL)
	{

		/* right operator */
		ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
		temp = right_oper(opname, ltypeId);
		opform = (OperatorTupleForm) GETSTRUCT(temp);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = NULL;

	}
	else if (ltree == NULL)
	{

		/* left operator */
		rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);
		temp = left_oper(opname, rtypeId);
		opform = (OperatorTupleForm) GETSTRUCT(temp);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
		left = NULL;

	}
	else
	{
		char	   *outstr;
		Oid			infunc,
					outfunc;
		Type		newtype;

#define CONVERTABLE_TYPE(t) (	(t) == INT2OID || \
								(t) == INT4OID || \
								(t) == OIDOID || \
								(t) == FLOAT4OID || \
								(t) == FLOAT8OID || \
								(t) == CASHOID)

		/* binary operator */
		ltypeId = (ltree == NULL) ? UNKNOWNOID : exprType(ltree);
		rtypeId = (rtree == NULL) ? UNKNOWNOID : exprType(rtree);

		/*
		 * convert constant when using a const of a numeric type and a
		 * non-const of another numeric type
		 */
		if (CONVERTABLE_TYPE(ltypeId) && nodeTag(ltree) != T_Const &&
			CONVERTABLE_TYPE(rtypeId) && nodeTag(rtree) == T_Const &&
			!((Const *) rtree)->constiscast)
		{
			outfunc = typeid_get_retoutfunc(rtypeId);
			infunc = typeid_get_retinfunc(ltypeId);
			outstr = (char *) fmgr(outfunc, ((Const *) rtree)->constvalue);
			((Const *) rtree)->constvalue = (Datum) fmgr(infunc, outstr);
			pfree(outstr);
			((Const *) rtree)->consttype = rtypeId = ltypeId;
			newtype = get_id_type(rtypeId);
			((Const *) rtree)->constlen = tlen(newtype);
			((Const *) rtree)->constbyval = tbyval(newtype);
		}

		if (CONVERTABLE_TYPE(rtypeId) && nodeTag(rtree) != T_Const &&
			CONVERTABLE_TYPE(ltypeId) && nodeTag(ltree) == T_Const &&
			!((Const *) ltree)->constiscast)
		{
			outfunc = typeid_get_retoutfunc(ltypeId);
			infunc = typeid_get_retinfunc(rtypeId);
			outstr = (char *) fmgr(outfunc, ((Const *) ltree)->constvalue);
			((Const *) ltree)->constvalue = (Datum) fmgr(infunc, outstr);
			pfree(outstr);
			((Const *) ltree)->consttype = ltypeId = rtypeId;
			newtype = get_id_type(ltypeId);
			((Const *) ltree)->constlen = tlen(newtype);
			((Const *) ltree)->constbyval = tbyval(newtype);
		}

		temp = oper(opname, ltypeId, rtypeId, false);
		opform = (OperatorTupleForm) GETSTRUCT(temp);
		left = make_operand(opname, ltree, ltypeId, opform->oprleft);
		right = make_operand(opname, rtree, rtypeId, opform->oprright);
	}

	newop = makeOper(oprid(temp),		/* opno */
					 InvalidOid,/* opid */
					 opform->oprresult, /* operator result type */
					 0,
					 NULL);

	result = makeNode(Expr);
	result->typeOid = opform->oprresult;
	result->opType = OP_EXPR;
	result->oper = (Node *) newop;

	if (!left)
	{
		result->args = lcons(right, NIL);
	}
	else if (!right)
	{
		result->args = lcons(left, NIL);
	}
	else
	{
		result->args = lcons(left, lcons(right, NIL));
	}

	return result;
}

Oid
find_atttype(Oid relid, char *attrname)
{
	int			attid;
	Oid			vartype;
	Relation	rd;

	rd = heap_open(relid);
	if (!RelationIsValid(rd))
	{
		rd = heap_openr(tname(get_id_type(relid)));
		if (!RelationIsValid(rd))
			elog(WARN, "cannot compute type of att %s for relid %d",
				 attrname, relid);
	}

	attid = nf_varattno(rd, attrname);

	if (attid == InvalidAttrNumber)
		elog(WARN, "Invalid attribute %s\n", attrname);

	vartype = att_typeid(rd, attid);

	/*
	 * close relation we're done with it now
	 */
	heap_close(rd);

	return (vartype);
}


Var		   *
make_var(ParseState *pstate, char *refname, char *attrname, Oid *type_id)
{
	Var		   *varnode;
	int			vnum,
				attid;
	Oid			vartypeid;
	Relation	rd;
	RangeTblEntry *rte;

	rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	if (rte == NULL)
		rte = addRangeTableEntry(pstate, refname, refname, FALSE, FALSE, NULL);

	vnum = refnameRangeTablePosn(pstate->p_rtable, refname);

	rd = heap_open(rte->relid);

	attid = nf_varattno(rd, attrname);
	if (attid == InvalidAttrNumber)
		elog(WARN, "Invalid attribute %s\n", attrname);
	vartypeid = att_typeid(rd, attid);

	varnode = makeVar(vnum, attid, vartypeid, vnum, attid);

	heap_close(rd);

	*type_id = vartypeid;
	return varnode;
}

/*
 *	make_array_ref() -- Make an array reference node.
 *
 *		Array references can hang off of arbitrary nested dot (or
 *		function invocation) expressions.  This routine takes a
 *		tree generated by ParseFunc() and an array index and
 *		generates a new array reference tree.  We do some simple
 *		typechecking to be sure the dereference is valid in the
 *		type system, but we don't do any bounds checking here.
 *
 *	indirection is a list of A_Indices
 */
ArrayRef   *
make_array_ref(Node *expr,
			   List *indirection)
{
	Oid			typearray;
	HeapTuple	type_tuple;
	TypeTupleForm type_struct_array,
				type_struct_element;
	ArrayRef   *aref;
	Oid			reftype;
	List	   *upperIndexpr = NIL;
	List	   *lowerIndexpr = NIL;

	typearray = exprType(expr);

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(WARN, "make_array_ref: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}

	/* get the type tuple for the element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
							ObjectIdGetDatum(type_struct_array->typelem),
									 0, 0, 0);
	if (!HeapTupleIsValid(type_tuple))
		elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	type_struct_element = (TypeTupleForm) GETSTRUCT(type_tuple);

	while (indirection != NIL)
	{
		A_Indices  *ind = lfirst(indirection);

		if (ind->lidx)
		{

			/*
			 * XXX assumes all lower indices non null in this case
			 */
			lowerIndexpr = lappend(lowerIndexpr, ind->lidx);
		}
		upperIndexpr = lappend(upperIndexpr, ind->uidx);
		indirection = lnext(indirection);
	}
	aref = makeNode(ArrayRef);
	aref->refattrlength = type_struct_array->typlen;
	aref->refelemlength = type_struct_element->typlen;
	aref->refelemtype = type_struct_array->typelem;
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = expr;
	aref->refassgnexpr = NULL;

	if (lowerIndexpr == NIL)	/* accessing a single array element */
		reftype = aref->refelemtype;
	else
/* request to clip a part of the array, the result is another array */
		reftype = typearray;

	/*
	 * we change it to reflect the true type; since the original
	 * refelemtype doesn't seem to get used anywhere. - ay 10/94
	 */
	aref->refelemtype = reftype;

	return aref;
}

ArrayRef   *
make_array_set(Expr *target_expr,
			   List *upperIndexpr,
			   List *lowerIndexpr,
			   Expr *expr)
{
	Oid			typearray;
	HeapTuple	type_tuple;
	TypeTupleForm type_struct_array;
	TypeTupleForm type_struct_element;
	ArrayRef   *aref;
	Oid			reftype;

	typearray = exprType((Node *) target_expr);

	type_tuple = SearchSysCacheTuple(TYPOID,
									 ObjectIdGetDatum(typearray),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	/* get the array type struct from the type tuple */
	type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);

	if (type_struct_array->typelem == InvalidOid)
	{
		elog(WARN, "make_array_ref: type %s is not an array",
			 (Name) &(type_struct_array->typname.data[0]));
	}
	/* get the type tuple for the element type */
	type_tuple = SearchSysCacheTuple(TYPOID,
							ObjectIdGetDatum(type_struct_array->typelem),
									 0, 0, 0);

	if (!HeapTupleIsValid(type_tuple))
		elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
			 typearray);

	type_struct_element = (TypeTupleForm) GETSTRUCT(type_tuple);

	aref = makeNode(ArrayRef);
	aref->refattrlength = type_struct_array->typlen;
	aref->refelemlength = type_struct_element->typlen;
	aref->refelemtype = type_struct_array->typelem;
	aref->refelembyval = type_struct_element->typbyval;
	aref->refupperindexpr = upperIndexpr;
	aref->reflowerindexpr = lowerIndexpr;
	aref->refexpr = (Node *) target_expr;
	aref->refassgnexpr = (Node *) expr;

	if (lowerIndexpr == NIL)	/* accessing a single array element */
		reftype = aref->refelemtype;
	else
/* request to set a part of the array, by another array */
		reftype = typearray;

	aref->refelemtype = reftype;

	return aref;
}

/*
 *
 * make_const -
 *
 * - takes a lispvalue, (as returned to the yacc routine by the lexer)
 *	 extracts the type, and makes the appropriate type constant
 *	 by invoking the (c-callable) lisp routine c-make-const
 *	 via the lisp_call() mechanism
 *
 * eventually, produces a "const" lisp-struct as per nodedefs.cl
 */
Const	   *
make_const(Value *value)
{
	Type		tp;
	Datum		val;
	Const	   *con;

	switch (nodeTag(value))
	{
		case T_Integer:
			tp = type("int4");
			val = Int32GetDatum(intVal(value));
			break;

		case T_Float:
			{
				float64		dummy;

				tp = type("float8");

				dummy = (float64) palloc(sizeof(float64data));
				*dummy = floatVal(value);

				val = Float64GetDatum(dummy);
			}
			break;

		case T_String:
			tp = type("unknown");		/* unknown for now, will be type
										 * coerced */
			val = PointerGetDatum(textin(strVal(value)));
			break;

		case T_Null:
		default:
			{
				if (nodeTag(value) != T_Null)
					elog(NOTICE, "unknown type : %d\n", nodeTag(value));

				/* null const */
				con = makeConst(0, 0, (Datum) NULL, true, false, false, false);
				return con;
			}
	}

	con = makeConst(typeid(tp),
					tlen(tp),
					val,
					false,
					tbyval(tp),
					false,		/* not a set */
					false);

	return (con);
}

/*
 * param_type_init()
 *
 * keep enough information around fill out the type of param nodes
 * used in postquel functions
 */
void
param_type_init(Oid *typev, int nargs)
{
	pfunc_num_args = nargs;
	param_type_info = typev;
}

Oid
param_type(int t)
{
	if ((t > pfunc_num_args) || (t == 0))
		return InvalidOid;
	return param_type_info[t - 1];
}

/*
 * handleTargetColname -
 *	  use column names from insert
 */
void
handleTargetColname(ParseState *pstate, char **resname,
					char *refname, char *colname)
{
	if (pstate->p_is_insert)
	{
		if (pstate->p_insert_columns != NIL)
		{
			Ident	   *id = lfirst(pstate->p_insert_columns);

			*resname = id->name;
			pstate->p_insert_columns = lnext(pstate->p_insert_columns);
		}
		else
			elog(WARN, "insert: more expressions than target columns");
	}
	if (pstate->p_is_insert || pstate->p_is_update)
		checkTargetTypes(pstate, *resname, refname, colname);
}

/*
 * checkTargetTypes -
 *	  checks value and target column types
 */
static void
checkTargetTypes(ParseState *pstate, char *target_colname,
				 char *refname, char *colname)
{
	Oid			attrtype_id,
				attrtype_target;
	int			resdomno_id,
				resdomno_target;
	Relation	rd;
	RangeTblEntry *rte;

	if (target_colname == NULL || colname == NULL)
		return;

	if (refname != NULL)
		rte = refnameRangeTableEntry(pstate->p_rtable, refname);
	else
	{
		rte = colnameRangeTableEntry(pstate, colname);
		if (rte == (RangeTblEntry *) NULL)
			elog(WARN, "attribute %s not found", colname);
		refname = rte->refname;
	}

/*
	if (pstate->p_is_insert && rte == pstate->p_target_rangetblentry)
		elog(WARN, "%s not available in this context", colname);
*/
	rd = heap_open(rte->relid);

	resdomno_id = varattno(rd, colname);
	attrtype_id = att_typeid(rd, resdomno_id);

	resdomno_target = varattno(pstate->p_target_relation, target_colname);
	attrtype_target = att_typeid(pstate->p_target_relation, resdomno_target);

	if (attrtype_id != attrtype_target)
		elog(WARN, "Type of %s does not match target column %s",
			 colname, target_colname);

	if ((attrtype_id == BPCHAROID || attrtype_id == VARCHAROID) &&
		rd->rd_att->attrs[resdomno_id - 1]->attlen !=
	pstate->p_target_relation->rd_att->attrs[resdomno_target - 1]->attlen)
		elog(WARN, "Length of %s does not match length of target column %s",
			 colname, target_colname);

	heap_close(rd);
}
