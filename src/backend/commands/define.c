/*-------------------------------------------------------------------------
 *
 * define.c
 *
 *	  These routines execute some of the CREATE statements.  In an earlier
 *	  version of Postgres, these were "define" statements.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/define.c,v 1.36 1999/10/02 21:33:24 tgl Exp $
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *		Most of the parse-tree manipulation routines are defined in
 *		commands/manip.c.
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <math.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "optimizer/cost.h"
#include "tcop/dest.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static char *defGetString(DefElem *def);
static double defGetNumeric(DefElem *def);
static int	defGetTypeLength(DefElem *def);

#define DEFAULT_TYPDELIM		','


static void
case_translate_language_name(const char *input, char *output)
{
/*-------------------------------------------------------------------------
  Translate the input language name to lower case, except if it's C,
  translate to upper case.
--------------------------------------------------------------------------*/
	int			i;

	for (i = 0; i < NAMEDATALEN && input[i]; ++i)
		output[i] = tolower(input[i]);

	output[i] = '\0';

	if (strcmp(output, "c") == 0)
		output[0] = 'C';
}



static void
compute_return_type(const Node *returnType,
					char **prorettype_p, bool *returnsSet_p)
{
/*---------------------------------------------------------------------------
   Examine the "returns" clause returnType of the CREATE FUNCTION statement
   and return information about it as **prorettype_p and **returnsSet.
----------------------------------------------------------------------------*/
	if (nodeTag(returnType) == T_TypeName)
	{
		/* a set of values */
		TypeName   *setType = (TypeName *) returnType;

		*prorettype_p = setType->name;
		*returnsSet_p = setType->setof;
	}
	else
	{
		/* singleton */
		*prorettype_p = strVal(returnType);
		*returnsSet_p = false;
	}
}


static void
compute_full_attributes(List *parameters, int32 *byte_pct_p,
						int32 *perbyte_cpu_p, int32 *percall_cpu_p,
						int32 *outin_ratio_p, bool *canCache_p)
{
/*--------------------------------------------------------------------------
  Interpret the parameters *parameters and return their contents as
  *byte_pct_p, etc.

  These parameters supply optional information about a function.
  All have defaults if not specified.

  Note: as of version 6.6, canCache is used (if set, the optimizer's
  constant-folder is allowed to pre-evaluate the function if all its
  inputs are constant).  The other four are not used.  They used to be
  used in the "expensive functions" optimizer, but that's been dead code
  for a long time.

  Since canCache is useful for any function, we now allow attributes to be
  supplied for all functions regardless of language.
---------------------------------------------------------------------------*/
	List	   *pl;

	/* the defaults */
	*byte_pct_p = BYTE_PCT;
	*perbyte_cpu_p = PERBYTE_CPU;
	*percall_cpu_p = PERCALL_CPU;
	*outin_ratio_p = OUTIN_RATIO;
	*canCache_p = false;

	foreach(pl, parameters)
	{
		DefElem *param = (DefElem *) lfirst(pl);

		if (strcasecmp(param->defname, "iscachable") == 0)
			*canCache_p = true;
		else if (strcasecmp(param->defname, "trusted") == 0)
		{
			/*
			 * we don't have untrusted functions any more. The 4.2
			 * implementation is lousy anyway so I took it out. -ay 10/94
			 */
			elog(ERROR, "untrusted function has been decommissioned.");
		}
		else if (strcasecmp(param->defname, "byte_pct") == 0)
			*byte_pct_p = (int) defGetNumeric(param);
		else if (strcasecmp(param->defname, "perbyte_cpu") == 0)
			*perbyte_cpu_p = (int) defGetNumeric(param);
		else if (strcasecmp(param->defname, "percall_cpu") == 0)
			*percall_cpu_p = (int) defGetNumeric(param);
		else if (strcasecmp(param->defname, "outin_ratio") == 0)
			*outin_ratio_p = (int) defGetNumeric(param);
		else
			elog(NOTICE, "Unrecognized function attribute '%s' ignored",
				 param->defname);
	}
}


/*
 * For a dynamically linked C language object, the form of the clause is
 *
 *	   AS <object file name> [, <link symbol name> ]
 *
 * In all other cases
 *
 *	   AS <object reference, or sql code>
 *
 */

static void
interpret_AS_clause(const char *languageName, const List *as,
					char **prosrc_str_p, char **probin_str_p)
{
	Assert(as != NIL);

	if (strcmp(languageName, "C") == 0)
	{

		/*
		 * For "C" language, store the file name in probin and, when
		 * given, the link symbol name in prosrc.
		 */
		*probin_str_p = strVal(lfirst(as));
		if (lnext(as) == NULL)
			*prosrc_str_p = "-";
		else
			*prosrc_str_p = strVal(lsecond(as));
	}
	else
	{
		/* Everything else wants the given string in prosrc. */
		*prosrc_str_p = strVal(lfirst(as));
		*probin_str_p = "-";

		if (lnext(as) != NULL)
			elog(ERROR, "CREATE FUNCTION: only one AS item needed for %s language",
				 languageName);
	}
}



/*
 * CreateFunction
 *	 Execute a CREATE FUNCTION utility statement.
 *
 */
void
CreateFunction(ProcedureStmt *stmt, CommandDest dest)
{
	char	   *probin_str;

	/* pathname of executable file that executes this function, if any */
	char	   *prosrc_str;

	/* SQL that executes this function, if any */
	char	   *prorettype;

	/* Type of return value (or member of set of values) from function */
	char		languageName[NAMEDATALEN];

	/*
	 * name of language of function, with case adjusted: "C", "internal",
	 * or "SQL"
	 */

	bool		returnsSet;
	/* The function returns a set of values, as opposed to a singleton. */

	bool		lanisPL = false;

	/*
	 * The following are optional user-supplied attributes of the function.
	 */
	int32		byte_pct,
				perbyte_cpu,
				percall_cpu,
				outin_ratio;
	bool		canCache;


	case_translate_language_name(stmt->language, languageName);

	if (strcmp(languageName, "C") == 0 ||
		strcmp(languageName, "internal") == 0)
	{
		if (!superuser())
			elog(ERROR,
				 "Only users with Postgres superuser privilege are "
				 "permitted to create a function "
				 "in the '%s' language.  Others may use the 'sql' language "
				 "or the created procedural languages.",
				 languageName);
	}
	else if (strcmp(languageName, "sql") == 0)
	{
		/* No security check needed for SQL functions */
	}
	else
	{
		HeapTuple	languageTuple;
		Form_pg_language languageStruct;

		/* Lookup the language in the system cache */
		languageTuple = SearchSysCacheTuple(LANNAME,
											PointerGetDatum(languageName),
											0, 0, 0);

		if (!HeapTupleIsValid(languageTuple))
		{

			elog(ERROR,
				 "Unrecognized language specified in a CREATE FUNCTION: "
				 "'%s'.  Recognized languages are sql, C, internal "
				 "and the created procedural languages.",
				 languageName);
		}

		/* Check that this language is a PL */
		languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
		if (!(languageStruct->lanispl))
		{
			elog(ERROR,
				 "Language '%s' isn't defined as PL", languageName);
		}

		/*
		 * Functions in untrusted procedural languages are restricted to
		 * be defined by postgres superusers only
		 */
		if (languageStruct->lanpltrusted == false && !superuser())
		{
			elog(ERROR, "Only users with Postgres superuser privilege "
				 "are permitted to create a function in the '%s' "
				 "language.",
				 languageName);
		}

		lanisPL = true;
	}

	compute_return_type(stmt->returnType, &prorettype, &returnsSet);

	compute_full_attributes(stmt->withClause,
							&byte_pct, &perbyte_cpu, &percall_cpu,
							&outin_ratio, &canCache);

	interpret_AS_clause(languageName, stmt->as, &prosrc_str, &probin_str);

	/*
	 * And now that we have all the parameters, and know we're
	 * permitted to do so, go ahead and create the function.
	 */
	ProcedureCreate(stmt->funcname,
					returnsSet,
					prorettype,
					languageName,
					prosrc_str,		/* converted to text later */
					probin_str,		/* converted to text later */
					canCache,
					true,			/* (obsolete "trusted") */
					byte_pct,
					perbyte_cpu,
					percall_cpu,
					outin_ratio,
					stmt->defArgs,
					dest);
}



/* --------------------------------
 * DefineOperator
 *
 *		this function extracts all the information from the
 *		parameter list generated by the parser and then has
 *		OperatorCreate() do all the actual work.
 *
 * 'parameters' is a list of DefElem
 * --------------------------------
 */
void
DefineOperator(char *oprName,
			   List *parameters)
{
	uint16		precedence = 0; /* operator precedence */
	bool		canHash = false;/* operator hashes */
	bool		isLeftAssociative = true;		/* operator is left
												 * associative */
	char	   *functionName = NULL;	/* function for operator */
	char	   *typeName1 = NULL;		/* first type name */
	char	   *typeName2 = NULL;		/* second type name */
	char	   *commutatorName = NULL;	/* optional commutator operator
										 * name */
	char	   *negatorName = NULL;		/* optional negator operator name */
	char	   *restrictionName = NULL; /* optional restrict. sel.
										 * procedure */
	char	   *joinName = NULL;/* optional join sel. procedure name */
	char	   *sortName1 = NULL;		/* optional first sort operator */
	char	   *sortName2 = NULL;		/* optional second sort operator */
	List	   *pl;

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (!strcasecmp(defel->defname, "leftarg"))
		{
			/* see gram.y, must be setof */
			if (nodeTag(defel->arg) == T_TypeName)
				elog(ERROR, "setof type not implemented for leftarg");

			if (nodeTag(defel->arg) == T_String)
				typeName1 = defGetString(defel);
			else
				elog(ERROR, "type for leftarg is malformed.");
		}
		else if (!strcasecmp(defel->defname, "rightarg"))
		{
			/* see gram.y, must be setof */
			if (nodeTag(defel->arg) == T_TypeName)
				elog(ERROR, "setof type not implemented for rightarg");

			if (nodeTag(defel->arg) == T_String)
				typeName2 = defGetString(defel);
			else
				elog(ERROR, "type for rightarg is malformed.");
		}
		else if (!strcasecmp(defel->defname, "procedure"))
			functionName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "precedence"))
		{
			/* NOT IMPLEMENTED (never worked in v4.2) */
			elog(NOTICE, "CREATE OPERATOR: precedence not implemented");
		}
		else if (!strcasecmp(defel->defname, "associativity"))
		{
			/* NOT IMPLEMENTED (never worked in v4.2) */
			elog(NOTICE, "CREATE OPERATOR: associativity not implemented");
		}
		else if (!strcasecmp(defel->defname, "commutator"))
			commutatorName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "negator"))
			negatorName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "restrict"))
			restrictionName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "join"))
			joinName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "hashes"))
			canHash = TRUE;
		else if (!strcasecmp(defel->defname, "sort1"))
		{
			/* ----------------
			 * XXX ( ... [ , sort1 = oprname ] [ , sort2 = oprname ] ... )
			 * XXX is undocumented in the reference manual source as of
			 * 89/8/22.
			 * ----------------
			 */
			sortName1 = defGetString(defel);
		}
		else if (!strcasecmp(defel->defname, "sort2"))
			sortName2 = defGetString(defel);
		else
		{
			elog(NOTICE, "DefineOperator: attribute \"%s\" not recognized",
				 defel->defname);
		}
	}

	/*
	 * make sure we have our required definitions
	 */
	if (functionName == NULL)
		elog(ERROR, "Define: \"procedure\" unspecified");

	/* ----------------
	 *	now have OperatorCreate do all the work..
	 * ----------------
	 */
	OperatorCreate(oprName,		/* operator name */
				   typeName1,	/* first type name */
				   typeName2,	/* second type name */
				   functionName,/* function for operator */
				   precedence,	/* operator precedence */
				   isLeftAssociative,	/* operator is left associative */
				   commutatorName,		/* optional commutator operator
										 * name */
				   negatorName, /* optional negator operator name */
				   restrictionName,		/* optional restrict. sel.
										 * procedure */
				   joinName,	/* optional join sel. procedure name */
				   canHash,		/* operator hashes */
				   sortName1,	/* optional first sort operator */
				   sortName2);	/* optional second sort operator */

}

/* -------------------
 *	DefineAggregate
 * ------------------
 */
void
DefineAggregate(char *aggName, List *parameters)

{
	char	   *stepfunc1Name = NULL;
	char	   *stepfunc2Name = NULL;
	char	   *finalfuncName = NULL;
	char	   *baseType = NULL;
	char	   *stepfunc1Type = NULL;
	char	   *stepfunc2Type = NULL;
	char	   *init1 = NULL;
	char	   *init2 = NULL;
	List	   *pl;

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1
		 */
		if (!strcasecmp(defel->defname, "sfunc1"))
			stepfunc1Name = defGetString(defel);
		else if (!strcasecmp(defel->defname, "basetype"))
			baseType = defGetString(defel);
		else if (!strcasecmp(defel->defname, "stype1"))
		{
			stepfunc1Type = defGetString(defel);

			/*
			 * sfunc2
			 */
		}
		else if (!strcasecmp(defel->defname, "sfunc2"))
			stepfunc2Name = defGetString(defel);
		else if (!strcasecmp(defel->defname, "stype2"))
		{
			stepfunc2Type = defGetString(defel);

			/*
			 * final
			 */
		}
		else if (!strcasecmp(defel->defname, "finalfunc"))
		{
			finalfuncName = defGetString(defel);

			/*
			 * initial conditions
			 */
		}
		else if (!strcasecmp(defel->defname, "initcond1"))
			init1 = defGetString(defel);
		else if (!strcasecmp(defel->defname, "initcond2"))
			init2 = defGetString(defel);
		else
		{
			elog(NOTICE, "DefineAggregate: attribute \"%s\" not recognized",
				 defel->defname);
		}
	}

	/*
	 * make sure we have our required definitions
	 */
	if (baseType == NULL)
		elog(ERROR, "Define: \"basetype\" unspecified");
	if (stepfunc1Name != NULL)
	{
		if (stepfunc1Type == NULL)
			elog(ERROR, "Define: \"stype1\" unspecified");
	}
	if (stepfunc2Name != NULL)
	{
		if (stepfunc2Type == NULL)
			elog(ERROR, "Define: \"stype2\" unspecified");
	}

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	AggregateCreate(aggName,	/* aggregate name */
					stepfunc1Name,		/* first step function name */
					stepfunc2Name,		/* second step function name */
					finalfuncName,		/* final function name */
					baseType,	/* type of object being aggregated */
					stepfunc1Type,		/* return type of first function */
					stepfunc2Type,		/* return type of second function */
					init1,		/* first initial condition */
					init2);		/* second initial condition */

	/* XXX free palloc'd memory */
}

/*
 * DefineType
 *		Registers a new type.
 *
 */
void
DefineType(char *typeName, List *parameters)
{
	int16		internalLength = 0;		/* int2 */
	int16		externalLength = 0;		/* int2 */
	char	   *elemName = NULL;
	char	   *inputName = NULL;
	char	   *outputName = NULL;
	char	   *sendName = NULL;
	char	   *receiveName = NULL;
	char	   *defaultValue = NULL;	/* Datum */
	bool		byValue = false;
	char		delimiter = DEFAULT_TYPDELIM;
	char	   *shadow_type;
	List	   *pl;
	char		alignment = 'i';/* default alignment */

	/*
	 * Type names can only be 15 characters long, so that the shadow type
	 * can be created using the 16th character as necessary.
	 */
	if (strlen(typeName) >= (NAMEDATALEN - 1))
	{
		elog(ERROR, "DefineType: type names must be %d characters or less",
			 NAMEDATALEN - 1);
	}

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (!strcasecmp(defel->defname, "internallength"))
			internalLength = defGetTypeLength(defel);
		else if (!strcasecmp(defel->defname, "externallength"))
			externalLength = defGetTypeLength(defel);
		else if (!strcasecmp(defel->defname, "input"))
			inputName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "output"))
			outputName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "send"))
			sendName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "delimiter"))
		{
			char	   *p = defGetString(defel);

			delimiter = p[0];
		}
		else if (!strcasecmp(defel->defname, "receive"))
			receiveName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "element"))
			elemName = defGetString(defel);
		else if (!strcasecmp(defel->defname, "default"))
			defaultValue = defGetString(defel);
		else if (!strcasecmp(defel->defname, "passedbyvalue"))
			byValue = true;
		else if (!strcasecmp(defel->defname, "alignment"))
		{
			char	   *a = defGetString(defel);

			if (!strcasecmp(a, "double"))
				alignment = 'd';
			else if (!strcasecmp(a, "int"))
				alignment = 'i';
			else
			{
				elog(ERROR, "DefineType: \"%s\" alignment  not recognized",
					 a);
			}
		}
		else
		{
			elog(NOTICE, "DefineType: attribute \"%s\" not recognized",
				 defel->defname);
		}
	}

	/*
	 * make sure we have our required definitions
	 */
	if (inputName == NULL)
		elog(ERROR, "Define: \"input\" unspecified");
	if (outputName == NULL)
		elog(ERROR, "Define: \"output\" unspecified");

	/* ----------------
	 *	now have TypeCreate do all the real work.
	 * ----------------
	 */
	TypeCreate(typeName,		/* type name */
			   InvalidOid,		/* relation oid (n/a here) */
			   internalLength,	/* internal size */
			   externalLength,	/* external size */
			   'b',				/* type-type (base type) */
			   delimiter,		/* array element delimiter */
			   inputName,		/* input procedure */
			   outputName,		/* output procedure */
			   receiveName,		/* receive procedure */
			   sendName,		/* send procedure */
			   elemName,		/* element type name */
			   defaultValue,	/* default type value */
			   byValue,			/* passed by value */
			   alignment);

	/* ----------------
	 *	When we create a true type (as opposed to a complex type)
	 *	we need to have an shadow array entry for it in pg_type as well.
	 * ----------------
	 */
	shadow_type = makeArrayTypeName(typeName);

	TypeCreate(shadow_type,		/* type name */
			   InvalidOid,		/* relation oid (n/a here) */
			   -1,				/* internal size */
			   -1,				/* external size */
			   'b',				/* type-type (base type) */
			   DEFAULT_TYPDELIM,/* array element delimiter */
			   "array_in",		/* input procedure */
			   "array_out",		/* output procedure */
			   "array_in",		/* receive procedure */
			   "array_out",		/* send procedure */
			   typeName,		/* element type name */
			   defaultValue,	/* default type value */
			   false,			/* never passed by value */
			   alignment);

	pfree(shadow_type);
}

static char *
defGetString(DefElem *def)
{
	if (nodeTag(def->arg) != T_String)
		elog(ERROR, "Define: \"%s\" = what?", def->defname);
	return strVal(def->arg);
}

static double
defGetNumeric(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a numeric value",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (double) intVal(def->arg);
		case T_Float:
			return floatVal(def->arg);
		default:
			elog(ERROR, "Define: \"%s\" requires a numeric value",
				 def->defname);
	}
	return 0;					/* keep compiler quiet */
}

static int
defGetTypeLength(DefElem *def)
{
	if (nodeTag(def->arg) == T_Integer)
		return intVal(def->arg);
	else if (nodeTag(def->arg) == T_String &&
			 !strcasecmp(strVal(def->arg), "variable"))
		return -1;				/* variable length */

	elog(ERROR, "Define: \"%s\" = what?", def->defname);
	return -1;
}
