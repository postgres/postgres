/*-------------------------------------------------------------------------
 *
 * define.c--
 *    POSTGRES "define" utility code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/define.c,v 1.2 1996/10/23 07:40:01 scrappy Exp $
 *
 * DESCRIPTION
 *    The "DefineFoo" routines take the parse tree and pick out the
 *    appropriate arguments/flags, passing the results to the
 *    corresponding "FooDefine" routines (in src/catalog) that do
 *    the actual catalog-munging.
 *
 * NOTES
 *    These things must be defined and committed in the following order:
 *	"define function":
 *		input/output, recv/send procedures
 *	"define type":
 *		type
 *	"define operator":
 *		operators
 *
 *	Most of the parse-tree manipulation routines are defined in
 *	commands/manip.c.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "utils/tqual.h"
#include "catalog/catname.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_operator.h"
#include "utils/syscache.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "fmgr.h"		/* for fmgr */

#include "utils/builtins.h"	/* prototype for textin() */

#include "utils/elog.h"
#include "utils/palloc.h"
#include "commands/defrem.h"
#include "optimizer/xfunc.h"
#include "tcop/dest.h"

static char *defGetString(DefElem *def);
static int  defGetTypeLength(DefElem *def);

#define	DEFAULT_TYPDELIM	','

/*
 * DefineFunction --
 *	Registers a new function.
 *
 */
void
DefineFunction(ProcedureStmt *stmt, CommandDest dest)
{
    List   	*parameters = stmt->withClause;
    char        *proname = stmt->funcname;
    char*	probin_str;
    char*	prosrc_str;
    char *prorettype;
    char *languageName;
    bool	canCache = FALSE;
    List	*argList;
    int32       byte_pct = 100, perbyte_cpu, percall_cpu, outin_ratio = 100;
    bool	returnsSet;
    int		i;
    
    /* ----------------
     * figure out the language and convert it to lowercase.
     * ----------------
     */
    languageName = stmt->language;
    for (i = 0; i < NAMEDATALEN && languageName[i]; ++i) {
	languageName[i] = tolower(languageName[i]);
    }
    
    /* ----------------
     * handle "returntype = X".  The function could return a singleton
     * value or a set of values.  Figure out which.
     * ----------------
     */
    if (nodeTag(stmt->returnType)==T_TypeName) {
	TypeName *setType = (TypeName *)stmt->returnType;
	/* a set of values */
	prorettype = setType->name,
	returnsSet = true;
    }else {
	/* singleton */
	prorettype = strVal(stmt->returnType);
	returnsSet = false;
    }
    
    /* Next attributes are only defined for C functions */
    if ( strcmp(languageName, "c") == 0 ||
	 strcmp(languageName, "internal") == 0 )  {
	List *pl;

	/* the defaults */
	byte_pct = BYTE_PCT;
	perbyte_cpu = PERBYTE_CPU;
	percall_cpu = PERCALL_CPU;
	outin_ratio = OUTIN_RATIO;

	foreach(pl, parameters) {
	    int count;
	    char *ptr;
	    ParamString *param = (ParamString*)lfirst(pl);

	    if (!strcasecmp(param->name, "isacachable")) {
		/* ----------------
		 * handle "[ iscachable ]": figure out if Postquel functions 
		 * are cacheable automagically?
		 * ----------------
		 */
		canCache = TRUE;
	    }else if (!strcasecmp(param->name, "trusted")) {
		/*
		 * we don't have untrusted functions any more. The 4.2
		 * implementation is lousy anyway so I took it out.
		 *				   	   -ay 10/94
		 */
		elog(WARN, "untrusted function has been decommissioned.");
	    }else if (!strcasecmp(param->name, "byte_pct")) {
		/*
		 ** handle expensive function parameters
		 */
		byte_pct = atoi(param->val);
	    }else if (!strcasecmp(param->name, "perbyte_cpu")) {
		if (!sscanf(param->val, "%d", &perbyte_cpu)) {
		    for (count = 0, ptr = param->val; *ptr != '\0'; ptr++) {
			if (*ptr == '!') {
			    count++;
			}
		    }
		    perbyte_cpu = (int) pow(10.0, (double) count);
		}
	    }else if (!strcasecmp(param->name, "percall_cpu")) {
		if (!sscanf(param->val, "%d", &percall_cpu)) {
		    for (count = 0, ptr = param->val; *ptr != '\0'; ptr++) {
			if (*ptr == '!') {
			    count++;
			}
		    }
		    percall_cpu = (int) pow(10.0, (double) count);
		}
	    }else if (!strcasecmp(param->name, "outin_ratio")) {
		outin_ratio = atoi(param->val);
	    }
	}
    } else if (!strcmp(languageName, "sql")) {
	/* query optimizer groks sql, these are meaningless */
	perbyte_cpu = percall_cpu = 0;
    } else {
	elog(WARN, "DefineFunction: language '%s' is not supported",
	     languageName);
    }
    
    /* ----------------
     * handle "[ arg is (...) ]"
     * XXX fix optional arg handling below
     * ----------------
     */
    argList = stmt->defArgs;
    
    if ( strcmp(languageName, "c") == 0 ||
	 strcmp(languageName, "internal") == 0 ) {
	prosrc_str = "-";
	probin_str = stmt->as;
    } else {
	prosrc_str = stmt->as;
	probin_str = "-";
    }
    
    /* C is stored uppercase in pg_language */
    if (!strcmp(languageName, "c")) {
	languageName[0] = 'C';
    }
    
    /* ----------------
     *	now have ProcedureDefine do all the work..
     * ----------------
     */
    ProcedureCreate(proname,
		    returnsSet,
		    prorettype,
		    languageName,
		    prosrc_str,		/* converted to text later */
		    probin_str,		/* converted to text later */
		    canCache,
		    TRUE,
		    byte_pct,
		    perbyte_cpu,
		    percall_cpu, 
		    outin_ratio,
		    argList,
		    dest);
    
}

/* --------------------------------
 * DefineOperator--
 *
 *	this function extracts all the information from the
 *	parameter list generated by the parser and then has
 *	OperatorCreate() do all the actual work.
 *
 * 'parameters' is a list of DefElem
 * --------------------------------
 */
void
DefineOperator(char *oprName,	
	       List *parameters)
{
    uint16 	precedence=0; 		/* operator precedence */
    bool	canHash=false; 		/* operator hashes */
    bool	isLeftAssociative=true;	/* operator is left associative */
    char *functionName=NULL; 	/* function for operator */
    char *typeName1=NULL;	 	/* first type name */
    char *typeName2=NULL;	 	/* second type name */
    char *commutatorName=NULL; 	/* optional commutator operator name */
    char *negatorName=NULL; 	/* optional negator operator name */
    char *restrictionName=NULL;	/* optional restrict. sel. procedure */
    char *joinName=NULL;	 	/* optional join sel. procedure name */
    char *sortName1=NULL;	 	/* optional first sort operator */
    char *sortName2=NULL;	 	/* optional second sort operator */
    List	*pl;

    /*
     * loop over the definition list and extract the information we need.
     */
    foreach (pl, parameters) {
	DefElem *defel = (DefElem *)lfirst(pl);

	if (!strcasecmp(defel->defname, "leftarg")) {
	    /* see gram.y, must be setof */
	    if (nodeTag(defel->arg)==T_TypeName) 
		elog(WARN, "setof type not implemented for leftarg");

	    if (nodeTag(defel->arg)==T_String) {
		typeName1 = defGetString(defel);
	    }else {
		elog(WARN, "type for leftarg is malformed.");
	    }
	} else if (!strcasecmp(defel->defname, "rightarg")) {
	    /* see gram.y, must be setof */
	    if (nodeTag(defel->arg)==T_TypeName) 
		elog(WARN, "setof type not implemented for rightarg");

	    if (nodeTag(defel->arg)==T_String) {
		typeName2 = defGetString(defel);
	    }else {
		elog(WARN, "type for rightarg is malformed.");
	    }
	} else if (!strcasecmp(defel->defname, "procedure")) {
	    functionName = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "precedence")) {
	    /* NOT IMPLEMENTED (never worked in v4.2) */
	    elog(NOTICE, "CREATE OPERATOR: precedence not implemented");
	} else if (!strcasecmp(defel->defname, "associativity")) {
	    /* NOT IMPLEMENTED (never worked in v4.2) */
	    elog(NOTICE, "CREATE OPERATOR: associativity not implemented");
	} else if (!strcasecmp(defel->defname, "commutator")) {
	    commutatorName = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "negator")) {
	    negatorName = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "restrict")) {
	    restrictionName = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "join")) {
	    joinName = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "hashes")) {
	    canHash = TRUE;
	} else if (!strcasecmp(defel->defname, "sort1")) {
	    /* ----------------
	     * XXX ( ... [ , sort1 = oprname ] [ , sort2 = oprname ] ... )
	     * XXX is undocumented in the reference manual source as of
	     * 89/8/22.
	     * ----------------
	     */
	    sortName1 = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "sort2")) {
	    sortName2 = defGetString(defel);
	} else {
	    elog(NOTICE, "DefineOperator: attribute \"%s\" not recognized",
		 defel->defname);
	}
    }

    /*
     * make sure we have our required definitions
     */
    if (functionName==NULL) {
	elog(WARN, "Define: \"procedure\" unspecified");
    }
    
    /* ----------------
     *	now have OperatorCreate do all the work..
     * ----------------
     */
    OperatorCreate(oprName,	/* operator name */
		   typeName1,		/* first type name */
		   typeName2,		/* second type name */
		   functionName,	/* function for operator */
		   precedence,		/* operator precedence */
		   isLeftAssociative,	/* operator is left associative */
		   commutatorName,	/* optional commutator operator name */
		   negatorName,		/* optional negator operator name */
		   restrictionName,	/* optional restrict. sel. procedure */
		   joinName,		/* optional join sel. procedure name */
		   canHash,		/* operator hashes */
		   sortName1,		/* optional first sort operator */
		   sortName2);		/* optional second sort operator */
    
}

/* -------------------
 *  DefineAggregate
 * ------------------
 */
void
DefineAggregate(char *aggName, List *parameters)

{
    char *stepfunc1Name = NULL;
    char *stepfunc2Name = NULL;
    char *finalfuncName = NULL;
    char *baseType = NULL;
    char *stepfunc1Type = NULL;
    char *stepfunc2Type = NULL;
    char *init1 = NULL;
    char *init2 = NULL;
    List *pl;
    
    foreach (pl, parameters) {
	DefElem *defel = (DefElem *)lfirst(pl);

	/*
	 * sfunc1
	 */
	if (!strcasecmp(defel->defname, "sfunc1")) {
	    stepfunc1Name = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "basetype")) {
	    baseType = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "stype1")) {
	    stepfunc1Type = defGetString(defel);

	/*
	 * sfunc2
	 */
	} else if (!strcasecmp(defel->defname, "sfunc2")) {
	    stepfunc2Name = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "stype2")) {
	    stepfunc2Type = defGetString(defel);
	/*
	 * final
	 */
	} else if (!strcasecmp(defel->defname, "finalfunc")) {
	    finalfuncName = defGetString(defel);
	/*
	 * initial conditions
	 */
	} else if (!strcasecmp(defel->defname, "initcond1")) {
	    init1 = defGetString(defel);
	} else if (!strcasecmp(defel->defname, "initcond2")) {
	    init2 = defGetString(defel);
	} else {
	    elog(NOTICE, "DefineAggregate: attribute \"%s\" not recognized",
		 defel->defname);
	}
    }

    /*
     * make sure we have our required definitions
     */
    if (baseType==NULL) 
	elog(WARN, "Define: \"basetype\" unspecified");
    if (stepfunc1Name!=NULL) {
	if (stepfunc1Type==NULL) 
	    elog(WARN, "Define: \"stype1\" unspecified");
    }
    if (stepfunc2Name!=NULL) {
	if (stepfunc2Type==NULL)
	    elog(WARN, "Define: \"stype2\" unspecified");
    }

    /*
     * Most of the argument-checking is done inside of AggregateCreate
     */
    AggregateCreate(aggName,         	/* aggregate name */
		    stepfunc1Name,	/* first step function name */
		    stepfunc2Name,	/* second step function name */
		    finalfuncName,	/* final function name */
		    baseType,		/* type of object being aggregated */
		    stepfunc1Type,	/* return type of first function */
		    stepfunc2Type,	/* return type of second function */
		    init1,	/* first initial condition */
		    init2);	/* second initial condition */
    
    /* XXX free palloc'd memory */
}

/*
 * DefineType --
 *	Registers a new type.
 *
 */
void
DefineType(char *typeName, List *parameters)
{
    int16		internalLength= 0;	/* int2 */
    int16		externalLength= 0;	/* int2 */
    char *elemName = NULL;
    char *inputName = NULL;
    char *outputName = NULL;
    char *sendName = NULL;
    char *receiveName = NULL;
    char		*defaultValue = NULL;	/* Datum */
    bool		byValue = false;
    char		delimiter = DEFAULT_TYPDELIM;
    char *shadow_type;
    List		*pl;
    char		alignment = 'i';	/* default alignment */

    /*
     * Type names can only be 15 characters long, so that the shadow type
     * can be created using the 16th character as necessary.
     */
    if (strlen(typeName) >= (NAMEDATALEN - 1)) {
	elog(WARN, "DefineType: type names must be %d characters or less",
	     NAMEDATALEN - 1);
    }

    foreach(pl, parameters) {
	DefElem *defel = (DefElem*)lfirst(pl);

	if (!strcasecmp(defel->defname, "internallength")) {
	    internalLength = defGetTypeLength(defel);
	}else if (!strcasecmp(defel->defname, "externallength")) {
	    externalLength = defGetTypeLength(defel);
	}else if (!strcasecmp(defel->defname, "input")) {
	    inputName = defGetString(defel);
	}else if (!strcasecmp(defel->defname, "output")) {
	    outputName = defGetString(defel);
	}else if (!strcasecmp(defel->defname, "send")) {
	    sendName = defGetString(defel);
	}else if (!strcasecmp(defel->defname, "delimiter")) {
	    char *p = defGetString(defel);
	    delimiter = p[0];
	}else if (!strcasecmp(defel->defname, "receive")) {
	    receiveName = defGetString(defel);
	}else if (!strcasecmp(defel->defname, "element")) {
	    elemName = defGetString(defel);
	}else if (!strcasecmp(defel->defname, "default")) {
	    defaultValue = defGetString(defel);
	}else if (!strcasecmp(defel->defname, "passedbyvalue")) {
	    byValue = true;
	}else if (!strcasecmp(defel->defname, "alignment")) {
	    char *a = defGetString(defel);
	    if (!strcasecmp(a, "double")) {
		alignment = 'd';
	    } else if (!strcasecmp(a, "int")) {
		alignment = 'i';
	    } else {
		elog(WARN, "DefineType: \"%s\" alignment  not recognized",
		     a);
	    }
	}else {
	    elog(NOTICE, "DefineType: attribute \"%s\" not recognized",
		 defel->defname);
	}
    }

    /*
     * make sure we have our required definitions
     */
    if (inputName==NULL)
	elog(WARN, "Define: \"input\" unspecified");
    if (outputName==NULL)
	elog(WARN, "Define: \"output\" unspecified");
    
    /* ----------------
     *	now have TypeCreate do all the real work.
     * ----------------
     */
    (void) TypeCreate(typeName,	/* type name */
		      InvalidOid,  /* relation oid (n/a here) */
		      internalLength,	/* internal size */
		      externalLength,	/* external size */
		      'b',		/* type-type (base type) */
		      delimiter,	/* array element delimiter */
		      inputName,	/* input procedure */
		      outputName,	/* output procedure */
		      sendName,		/* send procedure */
		      receiveName,	/* receive procedure */
		      elemName,		/* element type name */
		      defaultValue,	/* default type value */
		      byValue,		/* passed by value */
		      alignment);
    
    /* ----------------
     *  When we create a true type (as opposed to a complex type)
     *  we need to have an shadow array entry for it in pg_type as well.
     * ----------------
     */
    shadow_type = makeArrayTypeName(typeName);

    (void) TypeCreate(shadow_type,	/* type name */
		      InvalidOid,  /* relation oid (n/a here) */
		      -1,		/* internal size */
		      -1,		/* external size */
		      'b',		/* type-type (base type) */
		      DEFAULT_TYPDELIM,	/* array element delimiter */
		      "array_in",		/* input procedure */
		      "array_out",		/* output procedure */
		      "array_out",		/* send procedure */
		      "array_in",		/* receive procedure */
		      typeName,	/* element type name */
		      defaultValue,	/* default type value */
		      false,		/* never passed by value */
		      alignment);
    
    pfree(shadow_type);
}

static char *
defGetString(DefElem *def)
{
    if (nodeTag(def->arg)!=T_String)
	elog(WARN, "Define: \"%s\" = what?", def->defname);
    return (strVal(def->arg));
}

static int 
defGetTypeLength(DefElem *def)
{
    if (nodeTag(def->arg)==T_Integer)
	return (intVal(def->arg));
    else if (nodeTag(def->arg)==T_String &&
	     !strcasecmp(strVal(def->arg),"variable"))
	return -1;	/* variable length */

    elog(WARN, "Define: \"%s\" = what?", def->defname);
    return -1;
}
