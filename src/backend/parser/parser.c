/*-------------------------------------------------------------------------
 *
 * ylib.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/parser.c,v 1.1.1.1 1996/07/09 06:21:40 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdio.h>
#ifndef WIN32
#include <pwd.h>
#endif /*WIN32 */
#include <sys/param.h>		/* for MAXPATHLEN */

#include "utils/elog.h"
#include "parser/catalog_utils.h"
#include "nodes/pg_list.h"
#include "utils/exc.h"
#include "utils/excid.h"
#include "utils/palloc.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/primnodes.h"
#include "nodes/plannodes.h"
#include "nodes/execnodes.h"
#include "nodes/relation.h"
#include "parser/parse_query.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "access/heapam.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h" 

char *parseString; /* the char* which holds the string to be parsed */
char *parseCh;     /* a pointer used during parsing to walk down ParseString*/

List *parsetree = NIL;

static void fixupsets();
static void define_sets();
/*
 * parser-- returns a list of parse trees
 * 
 *  CALLER is responsible for free'ing the list returned 
 */
QueryTreeList *
parser(char *str, Oid *typev, int nargs)
{
    QueryTreeList* queryList;
    int yyresult;

#if defined(FLEX_SCANNER)
    extern void DeleteBuffer(void);
#endif	/* FLEX_SCANNER */
    
    init_io();
    
    /* Set things up to read from the string, if there is one */
    if (strlen(str) != 0) {
	parseString = (char *) palloc(strlen(str) + 1);
	memmove(parseString,str,strlen(str)+1);
    }
    
    parser_init(typev, nargs);
    yyresult = yyparse();

#if defined(FLEX_SCANNER)
    DeleteBuffer();
#endif	/* FLEX_SCANNER */
    
    clearerr(stdin);
    
    if (yyresult) {	/* error */
	return((QueryTreeList*)NULL);
    }

    queryList = parse_analyze(parsetree);
    
#ifdef SETS_FIXED
    /* Fixing up sets calls the parser, so it reassigns the global
     * variable parsetree. So save the real parsetree.
     */
    savetree = parsetree;
    foreach (parse, savetree) {  /* savetree is really a list of parses */

	/* find set definitions embedded in query */
	fixupsets((Query *)lfirst(parse));  

    }
    return savetree;
#endif    

    return queryList;
}

static void
fixupsets(Query *parse)
{
    if (parse == NULL)
	return;
    if (parse->commandType==CMD_UTILITY)  /* utility */
	return;
    if (parse->commandType!=CMD_INSERT)
	return;
    define_sets(parse);
}

/* Recursively find all of the Consts in the parsetree.  Some of
 * these may represent a set.  The value of the Const will be the
 * query (a string) which defines the set.  Call SetDefine to define
 * the set, and store the OID of the new set in the Const instead.
 */
static void
define_sets(Node *clause)
{
#ifdef SETS_FIXED
    Oid setoid;
    Type t = type("oid");
    Oid typeoid = typeid(t);
    Size oidsize = tlen(t);
    bool oidbyval = tbyval(t);
    
    if (clause==NULL) {
	return;
    } else if (IsA(clause,LispList)) {
	define_sets(lfirst(clause));
	define_sets(lnext(clause));
    } else if (IsA(clause,Const)) {
	if (get_constisnull((Const)clause) || 
	    !get_constisset((Const)clause)) {
	    return;
	}
	setoid = SetDefine(((Const*)clause)->constvalue,
			   get_id_typname(((Const*)clause)->consttype));
	set_constvalue((Const)clause, setoid);
	set_consttype((Const)clause,typeoid);
	set_constlen((Const)clause,oidsize);
	set_constbyval((Const)clause,oidbyval);
    } else if ( IsA(clause,Iter) ) {
	define_sets(((Iter*)clause)->iterexpr);
    } else if (single_node (clause)) {
	return;
    } else if (or_clause(clause)) {
	List *temp;
	/* mapcan */
	foreach (temp, ((Expr*)clause)->args) {
	    define_sets(lfirst(temp));
	}
    } else if (is_funcclause (clause)) {
	List *temp;
	/* mapcan */
	foreach(temp, ((Expr*)clause)->args) {
	    define_sets(lfirst(temp));
	}
    } else if (IsA(clause,ArrayRef)) {
	define_sets(((ArrayRef*)clause)->refassgnexpr);
    } else if (not_clause (clause)) {
	define_sets (get_notclausearg (clause));
    } else if (is_opclause (clause)) {
	define_sets(get_leftop (clause));
	define_sets(get_rightop (clause));
    }
#endif
}

#define    PSIZE(PTR)      (*((int32 *)(PTR) - 1))
Node *
parser_typecast(Value *expr, TypeName *typename, int typlen)
{
    /* check for passing non-ints */
    Const *adt;
    Datum lcp;
    Type tp;
    char type_string[16];
    int32 len;
    char *cp = NULL;
    char *const_string; 
    bool string_palloced = false;

    switch(nodeTag(expr)) {
    case T_String:
	const_string = DatumGetPointer(expr->val.str);
	break;
    case T_Integer:
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string, "%ld", expr->val.ival);
	break;
    default:
	elog(WARN,
	     "parser_typecast: cannot cast this expression to type \"%s\"",
	     typename->name);
    }

    if (typename->arrayBounds != NIL) {
	sprintf(type_string,"_%s", typename->name);
	tp = (Type) type(type_string);
    } else {
	tp = (Type) type(typename->name);
    }
    
    len = tlen(tp);
    
#if 0 /* fix me */
    switch ( CInteger(lfirst(expr)) ) {
    case 23: /* int4 */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%d", ((Const*)lnext(expr))->constvalue);
	break;
	
    case 19: /* char16 */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%s", ((Const*)lnext(expr))->constvalue);
	break;
	
    case 18: /* char */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%c", ((Const)lnext(expr))->constvalue);
	break;
	
    case 701:/* float8 */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%f", ((Const)lnext(expr))->constvalue);
	break;
	
    case 25: /* text */
	const_string = DatumGetPointer(((Const)lnext(expr))->constvalue);
	const_string = (char *) textout((struct varlena *)const_string);
	break;
	
    case 705: /* unknown */
        const_string = DatumGetPointer(((Const)lnext(expr))->constvalue);
        const_string = (char *) textout((struct varlena *)const_string);
        break;
	
    default:
	elog(WARN,"unknown type %d", CInteger(lfirst(expr)));
    }
#endif

    cp = instr2 (tp, const_string, typlen);
    
    if (!tbyvalue(tp)) {
	if (len >= 0 && len != PSIZE(cp)) {
	    char *pp;
	    pp = (char *) palloc(len);
	    memmove(pp, cp, len);
	    cp = pp;
	}
	lcp = PointerGetDatum(cp);
    } else {
	switch(len) {
	case 1:
	    lcp = Int8GetDatum(cp);
	    break;
	case 2:
	    lcp = Int16GetDatum(cp);
	    break;
	case 4:
	    lcp = Int32GetDatum(cp);
	    break;
	default:
	    lcp = PointerGetDatum(cp);
	    break;
	}
    }
    
    adt = makeConst(typeid(tp),
		    len,
		    (Datum)lcp ,
		    0,
		    tbyvalue(tp), 
		    0 /* not a set */);
    
    if (string_palloced)
	pfree(const_string);
    
    return (Node*)adt;
}

Node *
parser_typecast2(Node *expr, int exprType, Type tp, int typlen)
{
    /* check for passing non-ints */
    Const *adt;
    Datum lcp;
    int32 len = tlen(tp);
    char *cp = NULL;
    
    char *const_string; 
    bool string_palloced = false;

    Assert(IsA(expr,Const));
    
    switch (exprType) {
    case 23: /* int4 */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%d",
		(int) ((Const*)expr)->constvalue);
	break;
    case 19: /* char16 */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%s",
		(char*) ((Const*)expr)->constvalue);
	break;
    case 18: /* char */
	const_string = (char *) palloc(256);
	string_palloced = true;
	sprintf(const_string,"%c",
		(char) ((Const*)expr)->constvalue);
	break;
    case 700:	/* float4 */
	{
	    float32 floatVal = 
		DatumGetFloat32(((Const*)expr)->constvalue);
	    const_string = (char *) palloc(256);
	    string_palloced = true;
	    sprintf(const_string,"%f", *floatVal);
	    break;
	}
    case 701:/* float8 */
	{
	    float64 floatVal = 
		DatumGetFloat64(((Const*)expr)->constvalue);
	    const_string = (char *) palloc(256);
	    string_palloced = true;
	    sprintf(const_string,"%f", *floatVal);
	    break;
	}
    case 25: /* text */
	const_string = 
	    DatumGetPointer(((Const*)expr)->constvalue );
	const_string = (char *) textout((struct varlena *)const_string);
	break;
    case 705: /* unknown */
        const_string =
	    DatumGetPointer(((Const*)expr)->constvalue );
	const_string = (char *) textout((struct varlena *)const_string);
        break;
    default:
	elog(WARN,"unknown type%d ",exprType);
    }
    
    cp = instr2 (tp, const_string, typlen);
    
    
    if (!tbyvalue(tp)) {
	if (len >= 0 && len != PSIZE(cp)) {
	    char *pp;
	    pp = (char *) palloc(len);
	    memmove(pp, cp, len);
	    cp = pp;
	}
	lcp = PointerGetDatum(cp);
    } else {
	switch(len) {
	case 1:
	    lcp = Int8GetDatum(cp);
	    break;
	case 2:
	    lcp = Int16GetDatum(cp);
	    break;
	case 4:
	    lcp = Int32GetDatum(cp);
	    break;
	default:
	    lcp = PointerGetDatum(cp);
	    break;
	}
    }
    
    adt = makeConst((Oid)typeid(tp),
		    (Size)len,
		    (Datum)lcp,
		    0, 
		    0 /*was omitted*/,
		    0 /* not a set */);
    /*
      printf("adt %s : %d %d %d\n",CString(expr),typeid(tp) ,
      len,cp);
      */
    if (string_palloced) pfree(const_string);

    return ((Node*) adt);
}

Aggreg *
ParseAgg(char *aggname, Oid basetype, Node *target)
{
    Oid fintype;
    Oid vartype;
    Oid xfn1;
    Form_pg_aggregate aggform;
    Aggreg *aggreg;
    HeapTuple theAggTuple;
    
    theAggTuple = SearchSysCacheTuple(AGGNAME, PointerGetDatum(aggname),
				      ObjectIdGetDatum(basetype), 
				      0, 0); 
    if (!HeapTupleIsValid(theAggTuple)) {
        elog(WARN, "aggregate %s does not exist", aggname);
    }
    
    aggform = (Form_pg_aggregate) GETSTRUCT(theAggTuple);
    fintype = aggform->aggfinaltype;
    xfn1 = aggform->aggtransfn1;
    
    if (nodeTag(target) != T_Var)
	elog(WARN, "parser: aggregate can only be applied on an attribute");

    /* only aggregates with transfn1 need a base type */
    if (OidIsValid(xfn1)) {	
	basetype = aggform->aggbasetype;
	vartype = ((Var*)target)->vartype;

	if (basetype != vartype) {
	    Type tp1, tp2, get_id_type();
	    
	    tp1 = get_id_type(basetype);
	    tp2 = get_id_type(vartype);
	    elog(NOTICE, "Aggregate type mismatch:");
	    elog(WARN, "%s works on %s, not %s", aggname,
		 tname(tp1), tname(tp2));
        }
    }

    aggreg = makeNode(Aggreg);
    aggreg->aggname = pstrdup(aggname);
    aggreg->basetype = aggform->aggbasetype;
    aggreg->aggtype = fintype;

    aggreg->target = target;

    return aggreg;
}


    
