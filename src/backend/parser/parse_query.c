/*-------------------------------------------------------------------------
 *
 * parse_query.c--
 *    take an "optimizable" stmt and make the query tree that
 *     the planner requires.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/Attic/parse_query.c,v 1.2 1996/07/19 07:24:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>
#include "postgres.h"

#include "access/heapam.h"
#include "utils/tqual.h"
#include "access/tupmacs.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/acl.h"          /* for ACL_NO_PRIV_WARNING */
#include "utils/rel.h" 		/* Relation stuff */

#include "utils/syscache.h"
#include "catalog/pg_type.h"
#include "catalog_utils.h"
#include "parser/parse_query.h"
/* #include "parser/io.h" */
#include "utils/lsyscache.h"

#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"

Oid *param_type_info;
int pfunc_num_args;

extern int Quiet;


/* given range variable, return id of variable; position starts with 1 */
int
RangeTablePosn(List *rtable, char *rangevar)
{
    int index;
    List *temp;
    
    index = 1;
/*    temp = pstate->p_rtable; */
    temp = rtable;
    while (temp != NIL) {
	RangeTblEntry *rt_entry = lfirst(temp);

	if (!strcmp(rt_entry->refname, rangevar))
	    return index;

	temp = lnext(temp);
	index++;
    }
    return(0);
}

char*
VarnoGetRelname(ParseState *pstate, int vnum)
{
    int i;
    List *temp = pstate->p_rtable;
    for( i = 1; i < vnum ; i++) 
	temp = lnext(temp);
    return(((RangeTblEntry*)lfirst(temp))->relname);
}


RangeTblEntry *
makeRangeTableEntry(char *relname,
		    bool inh,
		    TimeRange *timeRange,
		    char *refname)
{
    Relation relation;
    RangeTblEntry *ent = makeNode(RangeTblEntry);

    ent->relname = pstrdup(relname);
    ent->refname = refname;

    relation = heap_openr(ent->relname);
    if (relation == NULL) {
	elog(WARN,"%s: %s",
	     relname, ACL_NO_PRIV_WARNING);
    }

    /*
     * Flags - zero or more from archive,inheritance,union,version
     *  or recursive (transitive closure)
     * [we don't support them all -- ay 9/94 ]
     */
    ent->inh = inh;

    ent->timeRange = timeRange;
    
    /* RelOID */
    ent->relid = RelationGetRelationId(relation);

    /*
     * close the relation we're done with it for now.
     */
    heap_close(relation);
    return ent;
}

/*
 * expandAll -
 *    makes a list of attributes
 *    assumes reldesc caching works
 */
List *
expandAll(ParseState* pstate, char *relname, int *this_resno)
{
    Relation rdesc;
    List *tall = NIL;
    Var *varnode;
    int i, maxattrs, first_resno;
    int type_id, type_len, vnum;
    char *physical_relname;
    
    first_resno = *this_resno;
    
    /* printf("\nExpanding %.*s.all\n", NAMEDATALEN, relname); */
    vnum = RangeTablePosn(pstate->p_rtable, relname);
    if ( vnum == 0 ) {
	pstate->p_rtable = lappend(pstate->p_rtable,
				   makeRangeTableEntry(relname, FALSE, NULL,
						       relname));
	vnum = RangeTablePosn(pstate->p_rtable, relname);
    }
    
    physical_relname = VarnoGetRelname(pstate, vnum);
    
    rdesc = heap_openr(physical_relname);
    
    if (rdesc == NULL ) {
	elog(WARN,"Unable to expand all -- heap_openr failed on %s",
	     physical_relname);
	return NIL;
    }
    maxattrs = RelationGetNumberOfAttributes(rdesc);
    
    for ( i = maxattrs-1 ; i > -1 ; --i ) {
	char *attrname;
	TargetEntry *rte = makeNode(TargetEntry);
	
	attrname = pstrdup ((rdesc->rd_att->attrs[i]->attname).data);
	varnode = (Var*)make_var(pstate, relname, attrname, &type_id);
	type_len = (int)tlen(get_id_type(type_id));

	/* Even if the elements making up a set are complex, the
	 * set itself is not. */
	
	rte->resdom = makeResdom((AttrNumber) i + first_resno, 
				 (Oid)type_id,
				 (Size)type_len,
				 attrname,
				 (Index)0,
				 (Oid)0,
				 0);
	rte->expr = (Node *)varnode;
	tall = lcons(rte, tall);
    }
    
    /*
     * Close the reldesc - we're done with it now
     */
    heap_close(rdesc);
    *this_resno = first_resno + maxattrs;
    return(tall);
}

TimeQual
makeTimeRange(char *datestring1,
	      char *datestring2,
	      int timecode)	/* 0 = snapshot , 1 = timerange */
{
    TimeQual	qual;
    AbsoluteTime t1,t2;
    
    switch (timecode) {
    case 0:
	if (datestring1 == NULL) {
	    elog(WARN, "MakeTimeRange: bad snapshot arg");
	}
	t1 = nabstimein(datestring1);
	if (!AbsoluteTimeIsValid(t1)) {
	    elog(WARN, "bad snapshot time: \"%s\"",
		 datestring1);
	}
	qual = TimeFormSnapshotTimeQual(t1);
	break;
    case 1:
	if (datestring1 == NULL) {
	    t1 = NOSTART_ABSTIME;
	} else {
	    t1 = nabstimein(datestring1);
	    if (!AbsoluteTimeIsValid(t1)) {
		elog(WARN,
		     "bad range start time: \"%s\"",
		     datestring1);
	    }
	}
	if (datestring2 == NULL) {
	    t2 = NOEND_ABSTIME;
	} else {
	    t2 = nabstimein(datestring2);
	    if (!AbsoluteTimeIsValid(t2)) {
		elog(WARN,
		     "bad range end time: \"%s\"",
		     datestring2);
	    }
	}
	qual = TimeFormRangedTimeQual(t1,t2);
	break;
    default:
	elog(WARN, "MakeTimeRange: internal parser error");
    }
    return qual;
}

static void
disallow_setop(char *op, Type optype, Node *operand)
{
    if (operand==NULL)
	return;
    
    if (nodeTag(operand) == T_Iter) {
	elog(NOTICE, "An operand to the '%s' operator returns a set of %s,",
	     op, tname(optype));
	elog(WARN, "but '%s' takes single values, not sets.",
	     op);
    }
}

static Node *
make_operand(char *opname,
	     Node *tree,
	     int orig_typeId,
	     int true_typeId)
{
    Node *result;
    Type true_type;
    Datum val;
    Oid infunc;
    
    if (tree != NULL) {
	result = tree;
	true_type = get_id_type(true_typeId);
	disallow_setop(opname, true_type, result);
	if (true_typeId != orig_typeId) {	/* must coerce */
	    Const *con= (Const *)result;

	    Assert(nodeTag(result)==T_Const);
	    val = (Datum)textout((struct varlena *)
				 con->constvalue);
	    infunc = typeid_get_retinfunc(true_typeId);
	    con = makeNode(Const);
	    con->consttype = true_typeId;
	    con->constlen = tlen(true_type);
	    con->constvalue = (Datum)fmgr(infunc,
					  val,
					  get_typelem(true_typeId),
					  -1 /* for varchar() type */);
	    con->constisnull = false;
	    con->constbyval = true;
	    con->constisset = false;
	    result = (Node *)con;
	}
    }else {
	Const *con= makeNode(Const);

	con->consttype = true_typeId;
	con->constlen = 0;
	con->constvalue = (Datum)(struct varlena *)NULL;
	con->constisnull = true;
	con->constbyval = true;
	con->constisset = false;
	result = (Node *)con;
    }
    
    return result;
}


Expr *
make_op(char *opname, Node *ltree, Node *rtree)
{
    int ltypeId, rtypeId;
    Operator temp;
    OperatorTupleForm opform;
    Oper *newop;
    Node *left, *right;
    Expr *result;
    
    if (rtree == NULL) {

	/* right operator */
	ltypeId = (ltree==NULL) ? UNKNOWNOID : exprType(ltree);
	temp = right_oper(opname, ltypeId);
	opform = (OperatorTupleForm) GETSTRUCT(temp);
	left = make_operand(opname, ltree, ltypeId, opform->oprleft);
	right = NULL;

    }else if (ltree == NULL) {

	/* left operator */
	rtypeId = (rtree==NULL) ? UNKNOWNOID : exprType(rtree);
	temp = left_oper(opname, rtypeId);
	opform = (OperatorTupleForm) GETSTRUCT(temp);
	right = make_operand(opname, rtree, rtypeId, opform->oprright);
	left = NULL;

    }else {

	/* binary operator */
	ltypeId = (ltree==NULL) ? UNKNOWNOID : exprType(ltree);
	rtypeId = (rtree==NULL) ? UNKNOWNOID : exprType(rtree);
	temp = oper(opname, ltypeId, rtypeId);
	opform = (OperatorTupleForm) GETSTRUCT(temp);
	left = make_operand(opname, ltree, ltypeId, opform->oprleft);
	right = make_operand(opname, rtree, rtypeId, opform->oprright);
    }

    newop = makeOper(oprid(temp),   	/* opno */
		     InvalidOid,	/* opid */
		     opform->oprresult, /* operator result type */
		     0,
		     NULL);

    result = makeNode(Expr);
    result->typeOid = opform->oprresult;
    result->opType = OP_EXPR;
    result->oper = (Node *)newop;

    if (!left) {
	result->args = lcons(right, NIL);
    } else if (!right) {
	result->args = lcons(left, NIL);
    } else {
	result->args = lcons(left, lcons(right, NIL));
    }

    return result;
}

int
find_atttype(Oid relid, char *attrname)
{
    int attid, vartype;
    Relation rd;
    
    rd = heap_open(relid);
    if (!RelationIsValid(rd)) {
	rd = heap_openr(tname(get_id_type(relid)));
	if (!RelationIsValid(rd))
	    elog(WARN, "cannot compute type of att %s for relid %d",
		 attrname, relid);
    }
    
    attid =  nf_varattno(rd, attrname);
    
    if (attid == InvalidAttrNumber) 
        elog(WARN, "Invalid attribute %s\n", attrname);
    
    vartype = att_typeid(rd , attid);
    
    /*
     * close relation we're done with it now
     */
    heap_close(rd);
    
    return (vartype);
}


Var *
make_var(ParseState *pstate, char *relname, char *attrname, int *type_id)
{
    Var *varnode;
    int vnum, attid, vartypeid;
    Relation rd;
    
    vnum = RangeTablePosn(pstate->p_rtable, relname);
    
    if (vnum == 0) {
	pstate->p_rtable =
	    lappend(pstate->p_rtable,
		     makeRangeTableEntry(relname, FALSE,
					 NULL, relname));
	vnum = RangeTablePosn (pstate->p_rtable, relname);
	relname = VarnoGetRelname(pstate, vnum);
    } else {
	relname = VarnoGetRelname(pstate, vnum);
    }
    
    rd = heap_openr(relname);
/*    relid = RelationGetRelationId(rd); */
    attid =  nf_varattno(rd, (char *) attrname);
    if (attid == InvalidAttrNumber) 
	elog(WARN, "Invalid attribute %s\n", attrname);
    vartypeid = att_typeid(rd, attid);

    varnode = makeVar(vnum, attid, vartypeid, vnum, attid);

    /*
     * close relation we're done with it now
     */
    heap_close(rd);

    *type_id = vartypeid;
    return varnode;
}

/*
 *  make_array_ref() -- Make an array reference node.
 *
 *	Array references can hang off of arbitrary nested dot (or
 *	function invocation) expressions.  This routine takes a
 *	tree generated by ParseFunc() and an array index and
 *	generates a new array reference tree.  We do some simple
 *	typechecking to be sure the dereference is valid in the
 *	type system, but we don't do any bounds checking here.
 *
 *  indirection is a list of A_Indices
 */
ArrayRef *
make_array_ref(Node *expr,
	       List *indirection)
{
    Oid typearray;
    HeapTuple type_tuple;
    TypeTupleForm type_struct_array, type_struct_element;
    ArrayRef *aref;
    int reftype;
    List *upperIndexpr=NIL;
    List *lowerIndexpr=NIL;
    
    typearray = (Oid) exprType(expr);
    
    type_tuple = SearchSysCacheTuple(TYPOID, 
				     ObjectIdGetDatum(typearray), 
				     0,0,0);
    
    if (!HeapTupleIsValid(type_tuple))
	elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
	     typearray);
    
    /* get the array type struct from the type tuple */
    type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);
    
    if (type_struct_array->typelem == InvalidOid) {
	elog(WARN, "make_array_ref: type %s is not an array",
	     (Name)&(type_struct_array->typname.data[0]));
    }
    
    /* get the type tuple for the element type */
    type_tuple = SearchSysCacheTuple(TYPOID, 
			     ObjectIdGetDatum(type_struct_array->typelem),
				     0,0,0);
    if (!HeapTupleIsValid(type_tuple))
	elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
	     typearray);
    
    type_struct_element = (TypeTupleForm) GETSTRUCT(type_tuple);

    while(indirection!=NIL) {
	A_Indices *ind = lfirst(indirection);
	if (ind->lidx) {
	    /* XXX assumes all lower indices non null in this case
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

    if (lowerIndexpr == NIL) /* accessing a single array element */
	reftype = aref->refelemtype;
    else /* request to clip a part of the array, the result is another array */
	reftype = typearray;

    /* we change it to reflect the true type; since the original refelemtype
     * doesn't seem to get used anywhere. - ay 10/94
     */
    aref->refelemtype = reftype;	

    return aref;
}

ArrayRef *
make_array_set(Expr *target_expr,
	       List *upperIndexpr,
	       List *lowerIndexpr,
	       Expr *expr)
{
    Oid typearray;
    HeapTuple type_tuple;
    TypeTupleForm type_struct_array;
    TypeTupleForm type_struct_element;
    ArrayRef *aref;
    int reftype;
    
    typearray = exprType((Node*)target_expr);
    
    type_tuple = SearchSysCacheTuple(TYPOID, 
				     ObjectIdGetDatum(typearray), 
				     0,0,0);
    
    if (!HeapTupleIsValid(type_tuple))
	elog(WARN, "make_array_ref: Cache lookup failed for type %d\n",
	     typearray);
    
    /* get the array type struct from the type tuple */
    type_struct_array = (TypeTupleForm) GETSTRUCT(type_tuple);
    
    if (type_struct_array->typelem == InvalidOid) {
	elog(WARN, "make_array_ref: type %s is not an array",
	     (Name)&(type_struct_array->typname.data[0]));
    }
    /* get the type tuple for the element type */
    type_tuple = SearchSysCacheTuple(TYPOID, 
				     ObjectIdGetDatum(type_struct_array->typelem),
				     0,0,0);
    
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
    aref->refexpr = (Node*)target_expr;
    aref->refassgnexpr = (Node*)expr;

    if (lowerIndexpr == NIL) /* accessing a single array element */
	reftype = aref->refelemtype;
    else /* request to set a part of the array, by another array */
	reftype = typearray;

    aref->refelemtype = reftype;
    
    return aref;
}

/*
 * 
 * make_const -
 * 
 * - takes a lispvalue, (as returned to the yacc routine by the lexer)
 *   extracts the type, and makes the appropriate type constant
 *   by invoking the (c-callable) lisp routine c-make-const
 *   via the lisp_call() mechanism
 *
 * eventually, produces a "const" lisp-struct as per nodedefs.cl
 */ 
Const *
make_const(Value *value)
{
    Type tp;
    Datum val;
    Const *con;
    
    switch(nodeTag(value)) {
    case T_Integer:
	tp = type("int4");
	val = Int32GetDatum(intVal(value));
	break;
	
    case T_Float:
	{
	    float32 dummy;
	    tp = type("float4");
	    
	    dummy = (float32)palloc(sizeof(float32data));
	    *dummy = floatVal(value);
	    
	    val = Float32GetDatum(dummy);
	}
	break;
	
    case T_String:
	tp = type("unknown"); /* unknown for now, will be type coerced */
	val = PointerGetDatum(textin(strVal(value)));
	break;

    case T_Null:
    default:
	{
	    if (nodeTag(value)!=T_Null)
		elog(NOTICE,"unknown type : %d\n", nodeTag(value));

	    /* null const */
	    con = makeConst(0, 0, (Datum)NULL, TRUE, 0, FALSE);
#ifdef NULL_PATCH
	    return con;
#else
	    return NULL /*con*/;
#endif
	}
    }

    con = makeConst(typeid(tp),
		    tlen(tp),
		    val,
		    FALSE,
		    tbyval(tp),
		    FALSE); /* not a set */

    return (con);
}

/*
 * param_type_init()
 *
 * keep enough information around fill out the type of param nodes
 * used in postquel functions
 */
void
param_type_init(Oid* typev, int nargs)
{
    pfunc_num_args = nargs;
    param_type_info = typev;
}

Oid
param_type(int t)
{
    if ((t >pfunc_num_args) ||(t ==0)) return InvalidOid;
    return param_type_info[t-1];
}

