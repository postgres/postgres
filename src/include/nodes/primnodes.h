/*-------------------------------------------------------------------------
 *
 * primnodes.h--
 *	  Definitions for parse tree/query tree ("primitive") nodes.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: primnodes.h,v 1.15 1998/01/19 18:11:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRIMNODES_H
#define PRIMNODES_H

#include <utils/fcache.h>
#include <access/attnum.h>
#include <nodes/pg_list.h>

/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/* ----------------
 * Resdom (Result Domain)
 *		resno			- attribute number
 *		restype			- type of the resdom
 *		reslen			- length (in bytes) of the result
 *		resname			- name of the resdom (could be NULL)
 *		reskey			- order of key in a sort (for those > 0)
 *		reskeyop		- sort operator Oid
 *		resjunk			- set to nonzero to eliminate the attribute
 *						  from final target list  e.g., ctid for replace
 *						  and delete
 *
 * ----------------
 */
typedef struct Resdom
{
	NodeTag		type;
	AttrNumber	resno;
	Oid			restype;
	int			reslen;
	char	   *resname;
	Index		reskey;
	Oid			reskeyop;
	int			resjunk;
} Resdom;

/* -------------
 * Fjoin
 *		initialized		- true if the Fjoin has already been initialized for
 *						  the current target list evaluation
 *		nNodes			- The number of Iter nodes returning sets that the
 *						  node will flatten
 *		outerList		- 1 or more Iter nodes
 *		inner			- exactly one Iter node.  We eval every node in the
 *						  outerList once then eval the inner node to completion
 *						  pair the outerList result vector with each inner
 *						  result to form the full result.  When the inner has
 *						  been exhausted, we get the next outer result vector
 *						  and reset the inner.
 *		results			- The complete (flattened) result vector
 *		alwaysNull		- a null vector to indicate sets with a cardinality of
 *						  0, we treat them as the set {NULL}.
 */
typedef struct Fjoin
{
	NodeTag		type;
	bool		fj_initialized;
	int			fj_nNodes;
	List	   *fj_innerNode;
	DatumPtr	fj_results;
	BoolPtr		fj_alwaysDone;
} Fjoin;

/* ----------------
 * Expr
 *		typeOid			- oid of the type of this expression
 *		opType			- type of this expression
 *		oper			- the Oper node if it is an OPER_EXPR or the
 *						  Func node if it is a FUNC_EXPR
 *		args			- arguments to this expression
 * ----------------
 */
typedef enum OpType
{
	OP_EXPR, FUNC_EXPR, OR_EXPR, AND_EXPR, NOT_EXPR
} OpType;

typedef struct Expr
{
	NodeTag		type;
	Oid			typeOid;		/* oid of the type of this expr */
	OpType		opType;			/* type of the op */
	Node	   *oper;			/* could be Oper or Func */
	List	   *args;			/* list of argument nodes */
} Expr;

/* ----------------
 * Var
 *		varno			- index of this var's relation in the range table
 *						  (could be INNER or OUTER)
 *		varattno		- attribute number of this var, or zero for all
 *		vartype			- pg_type tuple oid for the type of this var
 *		varnoold		- keep varno around in case it got changed to INNER/
 *						  OUTER (see match_varid)
 *		varoattno		- attribute number of this var
 *						  [ '(varnoold varoattno) was varid   -ay 2/95]
 * ----------------
 */
#define    INNER		65000
#define    OUTER		65001

#define    PRS2_CURRENT_VARNO			1
#define    PRS2_NEW_VARNO				2

typedef struct Var
{
	NodeTag		type;
	Index		varno;
	AttrNumber	varattno;
	Oid			vartype;
	Index		varnoold;		/* only used by optimizer */
	AttrNumber	varoattno;		/* only used by optimizer */
} Var;

/* ----------------
 * Oper
 *		opno			- PG_OPERATOR OID of the operator
 *		opid			- PG_PROC OID for the operator
 *		opresulttype	- PG_TYPE OID of the operator's return value
 *		opsize			- size of return result (cached by executor)
 *		op_fcache		- XXX comment me.
 *
 * ----
 * NOTE: in the good old days 'opno' used to be both (or either, or
 * neither) the pg_operator oid, and/or the pg_proc oid depending
 * on the postgres module in question (parser->pg_operator,
 * executor->pg_proc, planner->both), the mood of the programmer,
 * and the phase of the moon (rumors that it was also depending on the day
 * of the week are probably false). To make things even more postgres-like
 * (i.e. a mess) some comments were referring to 'opno' using the name
 * 'opid'. Anyway, now we have two separate fields, and of course that
 * immediately removes all bugs from the code...		[ sp :-) ].
 * ----------------
 */
typedef struct Oper
{
	NodeTag		type;
	Oid			opno;
	Oid			opid;
	Oid			opresulttype;
	int			opsize;
	FunctionCachePtr op_fcache;
} Oper;


/* ----------------
 * Const
 *		consttype - PG_TYPE OID of the constant's value
 *		constlen - length in bytes of the constant's value
 *		constvalue - the constant's value
 *		constisnull - whether the constant is null
 *				(if true, the other fields are undefined)
 *		constbyval - whether the information in constvalue
 *				if passed by value.  If true, then all the information
 *				is stored in the datum. If false, then the datum
 *				contains a pointer to the information.
 *		constisset - whether the const represents a set.  The const
 *				value corresponding will be the query that defines
 *				the set.
 * ----------------
 */
typedef struct Const
{
	NodeTag		type;
	Oid			consttype;
	Size		constlen;
	Datum		constvalue;
	bool		constisnull;
	bool		constbyval;
	bool		constisset;
	bool		constiscast;
} Const;

/* ----------------
 * Param
 *		paramkind - specifies the kind of parameter. The possible values
 *		for this field are specified in "params.h", and they are:
 *
 *		PARAM_NAMED: The parameter has a name, i.e. something
 *				like `$.salary' or `$.foobar'.
 *				In this case field `paramname' must be a valid Name.
 *
 *		PARAM_NUM:	 The parameter has only a numeric identifier,
 *				i.e. something like `$1', `$2' etc.
 *				The number is contained in the `paramid' field.
 *
 *		PARAM_NEW:	 Used in PRS2 rule, similar to PARAM_NAMED.
 *					 The `paramname' and `paramid' refer to the "NEW" tuple
 *					 The `pramname' is the attribute name and `paramid'
 *					 is the attribute number.
 *
 *		PARAM_OLD:	 Same as PARAM_NEW, but in this case we refer to
 *				the "OLD" tuple.
 *
 *		paramid - numeric identifier for literal-constant parameters ("$1")
 *		paramname - attribute name for tuple-substitution parameters ("$.foo")
 *		paramtype - PG_TYPE OID of the parameter's value
 *		param_tlist - allows for projection in a param node.
 * ----------------
 */
typedef struct Param
{
	NodeTag		type;
	int			paramkind;
	AttrNumber	paramid;
	char	   *paramname;
	Oid			paramtype;
	List	   *param_tlist;
} Param;


/* ----------------
 * Func
 *		funcid			- PG_FUNCTION OID of the function
 *		functype		- PG_TYPE OID of the function's return value
 *		funcisindex		- the function can be evaluated by scanning an index
 *						  (set during query optimization)
 *		funcsize		- size of return result (cached by executor)
 *		func_fcache		- runtime state while running this function.  Where
 *						  we are in the execution of the function if it
 *						  returns more than one value, etc.
 *						  See utils/fcache.h
 *		func_tlist		- projection of functions returning tuples
 *		func_planlist	- result of planning this func, if it's a PQ func
 * ----------------
 */
typedef struct Func
{
	NodeTag		type;
	Oid			funcid;
	Oid			functype;
	bool		funcisindex;
	int			funcsize;
	FunctionCachePtr func_fcache;
	List	   *func_tlist;
	List	   *func_planlist;
} Func;

/* ----------------
 * Aggreg
 *		aggname			- name of the aggregate
 *		basetype		- base type Oid of the aggregate
 *		aggtype			- type Oid of final result of the aggregate
 *		target			- attribute or expression we are aggregating on
 *		aggno			- index to ecxt_values
 * ----------------
 */
typedef struct Aggreg
{
	NodeTag		type;
	char	   *aggname;
	Oid			basetype;
	Oid			aggtype;
	Node	   *target;	
	int			aggno;
	bool		usenulls;
} Aggreg;

/* ----------------
 * SubLink
 *		subLinkType		- EXISTS, ALL, ANY, EXPR
 *		useor			- TRUE for <>
 *		lefthand		- list of Var/Const nodes on the left
 *		oper			- list of Oper nodes
 *		subselect		- subselect as Query* or parsetree
 * ----------------
 */
typedef enum SubLinkType
{
	EXISTS_SUBLINK, ALL_SUBLINK, ANY_SUBLINK, EXPR_SUBLINK
} SubLinkType;


typedef struct SubLink
{
	NodeTag		type;
	SubLinkType	subLinkType;
	bool		useor;
	List		*lefthand;
	List		*oper;
	Node		*subselect;
} SubLink;

/* ----------------
 * Array
 *		arrayelemtype	- base type of the array's elements (homogenous!)
 *		arrayelemlength - length of that type
 *		arrayelembyval	- can you pass this element by value?
 *		arrayndim		- number of dimensions of the array
 *		arraylow		- base for array indexing
 *		arrayhigh		- limit for array indexing
 *		arraylen		-
 * ----------------
 *
 *	memo from mao:	the array support we inherited from 3.1 is just
 *	wrong.	when time exists, we should redesign this stuff to get
 *	around a bunch of unfortunate implementation decisions made there.
 */
typedef struct Array
{
	NodeTag		type;
	Oid			arrayelemtype;
	int			arrayelemlength;
	bool		arrayelembyval;
	int			arrayndim;
	IntArray	arraylow;
	IntArray	arrayhigh;
	int			arraylen;
} Array;

/* ----------------
 *	ArrayRef:
 *		refelemtype		- type of the element referenced here
 *		refelemlength	- length of that type
 *		refelembyval	- can you pass this element type by value?
 *		refupperindexpr - expressions that evaluate to upper array index
 *		reflowerexpr- the expressions that evaluate to a lower array index
 *		refexpr			- the expression that evaluates to an array
 *		refassignexpr- the expression that evaluates to the new value
 *	to be assigned to the array in case of replace.
 * ----------------
 */
typedef struct ArrayRef
{
	NodeTag		type;
	int			refattrlength;
	int			refelemlength;
	Oid			refelemtype;
	bool		refelembyval;
	List	   *refupperindexpr;
	List	   *reflowerindexpr;
	Node	   *refexpr;
	Node	   *refassgnexpr;
} ArrayRef;

#endif							/* PRIMNODES_H */
