/*-------------------------------------------------------------------------
 *
 * define.c
 *
 *	  These routines execute some of the CREATE statements.  In an earlier
 *	  version of Postgres, these were "define" statements.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/define.c,v 1.70 2002/03/19 02:18:15 momjian Exp $
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
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "parser/parse_expr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static char *defGetString(DefElem *def);
static double defGetNumeric(DefElem *def);
static int	defGetTypeLength(DefElem *def);

#define DEFAULT_TYPDELIM		','


/*
 * Translate the input language name to lower case.
 */
static void
case_translate_language_name(const char *input, char *output)
{
	int			i;

	for (i = 0; i < NAMEDATALEN - 1 && input[i]; ++i)
		output[i] = tolower((unsigned char) input[i]);

	output[i] = '\0';
}



static void
compute_return_type(TypeName *returnType,
					char **prorettype_p, bool *returnsSet_p)
{
/*
 *	 Examine the "returns" clause returnType of the CREATE FUNCTION statement
 *	 and return information about it as *prorettype_p and *returnsSet.
 */
	*prorettype_p = TypeNameToInternalName(returnType);
	*returnsSet_p = returnType->setof;
}


static void
compute_full_attributes(List *parameters,
						int32 *byte_pct_p, int32 *perbyte_cpu_p,
						int32 *percall_cpu_p, int32 *outin_ratio_p,
						bool *canCache_p, bool *isStrict_p)
{
/*-------------
 *	 Interpret the parameters *parameters and return their contents as
 *	 *byte_pct_p, etc.
 *
 *	These parameters supply optional information about a function.
 *	All have defaults if not specified.
 *
 *	Note: currently, only two of these parameters actually do anything:
 *
 *	 * canCache means the optimizer's constant-folder is allowed to
 *	   pre-evaluate the function when all its inputs are constants.
 *
 *	 * isStrict means the function should not be called when any NULL
 *	   inputs are present; instead a NULL result value should be assumed.
 *
 *	The other four parameters are not used anywhere.	They used to be
 *	used in the "expensive functions" optimizer, but that's been dead code
 *	for a long time.
 *
 *	Since canCache and isStrict are useful for any function, we now allow
 *	attributes to be supplied for all functions regardless of language.
 *------------
 */
	List	   *pl;

	/* the defaults */
	*byte_pct_p = BYTE_PCT;
	*perbyte_cpu_p = PERBYTE_CPU;
	*percall_cpu_p = PERCALL_CPU;
	*outin_ratio_p = OUTIN_RATIO;
	*canCache_p = false;
	*isStrict_p = false;

	foreach(pl, parameters)
	{
		DefElem    *param = (DefElem *) lfirst(pl);

		if (strcasecmp(param->defname, "iscachable") == 0)
			*canCache_p = true;
		else if (strcasecmp(param->defname, "isstrict") == 0)
			*isStrict_p = true;
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
			elog(WARNING, "Unrecognized function attribute '%s' ignored",
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
interpret_AS_clause(Oid languageOid, const char *languageName, const List *as,
					char **prosrc_str_p, char **probin_str_p)
{
	Assert(as != NIL);

	if (languageOid == ClanguageId)
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

		if (lnext(as) != NIL)
			elog(ERROR, "CREATE FUNCTION: only one AS item needed for %s language",
				 languageName);
	}
}



/*
 * CreateFunction
 *	 Execute a CREATE FUNCTION utility statement.
 */
void
CreateFunction(ProcedureStmt *stmt)
{
	/* pathname of executable file that executes this function, if any */
	char	   *probin_str;
	/* SQL that executes this function, if any */
	char	   *prosrc_str;
	/* Type of return value (or member of set of values) from function */
	char	   *prorettype;
	/* name of language of function, with case adjusted */
	char		languageName[NAMEDATALEN];
	/* The function returns a set of values, as opposed to a singleton. */
	bool		returnsSet;
	/*
	 * The following are optional user-supplied attributes of the
	 * function.
	 */
	int32		byte_pct,
				perbyte_cpu,
				percall_cpu,
				outin_ratio;
	bool		canCache,
				isStrict;

	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	Oid languageOid;

	/* Convert language name to canonical case */
	case_translate_language_name(stmt->language, languageName);

	languageTuple = SearchSysCache(LANGNAME,
								   PointerGetDatum(languageName),
								   0, 0, 0);
	if (!HeapTupleIsValid(languageTuple))
		elog(ERROR, "language \"%s\" does not exist", languageName);

	languageOid = languageTuple->t_data->t_oid;
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	if (!((languageStruct->lanpltrusted
		   && pg_language_aclcheck(languageOid, GetUserId()) == ACLCHECK_OK)
		  || superuser()))
		elog(ERROR, "permission denied");

	ReleaseSysCache(languageTuple);

	/*
	 * Convert remaining parameters of CREATE to form wanted by
	 * ProcedureCreate.
	 */
	Assert(IsA(stmt->returnType, TypeName));
	compute_return_type((TypeName *) stmt->returnType,
						&prorettype, &returnsSet);

	compute_full_attributes(stmt->withClause,
							&byte_pct, &perbyte_cpu, &percall_cpu,
							&outin_ratio, &canCache, &isStrict);

	interpret_AS_clause(languageOid, languageName, stmt->as, &prosrc_str, &probin_str);

	/*
	 * And now that we have all the parameters, and know we're permitted
	 * to do so, go ahead and create the function.
	 */
	ProcedureCreate(stmt->funcname,
					stmt->replace,
					returnsSet,
					prorettype,
					languageOid,
					prosrc_str, /* converted to text later */
					probin_str, /* converted to text later */
					true,		/* (obsolete "trusted") */
					canCache,
					isStrict,
					byte_pct,
					perbyte_cpu,
					percall_cpu,
					outin_ratio,
					stmt->argTypes);
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
	bool		canHash = false;	/* operator hashes */
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
	char	   *joinName = NULL;	/* optional join sel. procedure name */
	char	   *sortName1 = NULL;		/* optional first sort operator */
	char	   *sortName2 = NULL;		/* optional second sort operator */
	List	   *pl;

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcasecmp(defel->defname, "leftarg") == 0)
		{
			typeName1 = defGetString(defel);
			if (IsA(defel->arg, TypeName) &&
				((TypeName *) defel->arg)->setof)
				elog(ERROR, "setof type not implemented for leftarg");
		}
		else if (strcasecmp(defel->defname, "rightarg") == 0)
		{
			typeName2 = defGetString(defel);
			if (IsA(defel->arg, TypeName) &&
				((TypeName *) defel->arg)->setof)
				elog(ERROR, "setof type not implemented for rightarg");
		}
		else if (strcasecmp(defel->defname, "procedure") == 0)
			functionName = defGetString(defel);
		else if (strcasecmp(defel->defname, "precedence") == 0)
		{
			/* NOT IMPLEMENTED (never worked in v4.2) */
			elog(NOTICE, "CREATE OPERATOR: precedence not implemented");
		}
		else if (strcasecmp(defel->defname, "associativity") == 0)
		{
			/* NOT IMPLEMENTED (never worked in v4.2) */
			elog(NOTICE, "CREATE OPERATOR: associativity not implemented");
		}
		else if (strcasecmp(defel->defname, "commutator") == 0)
			commutatorName = defGetString(defel);
		else if (strcasecmp(defel->defname, "negator") == 0)
			negatorName = defGetString(defel);
		else if (strcasecmp(defel->defname, "restrict") == 0)
			restrictionName = defGetString(defel);
		else if (strcasecmp(defel->defname, "join") == 0)
			joinName = defGetString(defel);
		else if (strcasecmp(defel->defname, "hashes") == 0)
			canHash = TRUE;
		else if (strcasecmp(defel->defname, "sort1") == 0)
		{
			/* ----------------
			 * XXX ( ... [ , sort1 = oprname ] [ , sort2 = oprname ] ... )
			 * XXX is undocumented in the reference manual source as of
			 * 89/8/22.
			 * ----------------
			 */
			sortName1 = defGetString(defel);
		}
		else if (strcasecmp(defel->defname, "sort2") == 0)
			sortName2 = defGetString(defel);
		else
		{
			elog(WARNING, "DefineOperator: attribute \"%s\" not recognized",
				 defel->defname);
		}
	}

	/*
	 * make sure we have our required definitions
	 */
	if (functionName == NULL)
		elog(ERROR, "Define: \"procedure\" unspecified");

	/*
	 * now have OperatorCreate do all the work..
	 */
	OperatorCreate(oprName,		/* operator name */
				   typeName1,	/* first type name */
				   typeName2,	/* second type name */
				   functionName,	/* function for operator */
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
	char	   *transfuncName = NULL;
	char	   *finalfuncName = NULL;
	char	   *baseType = NULL;
	char	   *transType = NULL;
	char	   *initval = NULL;
	List	   *pl;

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete
		 * spellings for sfunc, stype, initcond.
		 */
		if (strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetString(defel);
		else if (strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetString(defel);
		else if (strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetString(defel);
		else if (strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetString(defel);
		else if (strcasecmp(defel->defname, "stype") == 0)
			transType = defGetString(defel);
		else if (strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetString(defel);
		else if (strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else
			elog(WARNING, "DefineAggregate: attribute \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * make sure we have our required definitions
	 */
	if (baseType == NULL)
		elog(ERROR, "Define: \"basetype\" unspecified");
	if (transType == NULL)
		elog(ERROR, "Define: \"stype\" unspecified");
	if (transfuncName == NULL)
		elog(ERROR, "Define: \"sfunc\" unspecified");

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	AggregateCreate(aggName,	/* aggregate name */
					transfuncName,		/* step function name */
					finalfuncName,		/* final function name */
					baseType,	/* type of data being aggregated */
					transType,	/* transition data type */
					initval);	/* initial condition */
}

/*
 * DefineDomain
 *		Registers a new domain.
 */
void
DefineDomain(CreateDomainStmt *stmt)
{
	int16		internalLength = -1;	/* int2 */
	int16		externalLength = -1;	/* int2 */
	char	   *inputName = NULL;
	char	   *outputName = NULL;
	char	   *sendName = NULL;
	char	   *receiveName = NULL;

	/*
	 * Domains store the external representation in defaultValue
	 * and the interal Node representation in defaultValueBin
	 */
	char	   *defaultValue = NULL;
	char	   *defaultValueBin = NULL;

	bool		byValue = false;
	char		delimiter = DEFAULT_TYPDELIM;
	char		alignment = 'i';	/* default alignment */
	char		storage = 'p';	/* default TOAST storage method */
	char		typtype;
	Datum		datum;
	bool		typNotNull = false;
	char		*elemName = NULL;
	int32		typNDims = 0;	/* No array dimensions by default */

	bool		isnull;
	Relation	pg_type_rel;
	TupleDesc	pg_type_dsc;
	HeapTuple	typeTup;
	char	   *typeName = stmt->typename->name;

	List	   *listptr;
	List	   *schema = stmt->constraints;

	/*
	 * Domainnames, unlike typenames don't need to account for the '_'
	 * prefix.  So they can be one character longer.
	 */
	if (strlen(stmt->domainname) > (NAMEDATALEN - 1))
		elog(ERROR, "CREATE DOMAIN: domain names must be %d characters or less",
			 NAMEDATALEN - 1);


	/* Test for existing Domain (or type) of that name */
	typeTup = SearchSysCache( TYPENAME
							, PointerGetDatum(stmt->domainname)
							, 0, 0, 0
							);

	if (HeapTupleIsValid(typeTup))
	{
		elog(ERROR, "CREATE DOMAIN: domain or type  %s already exists",
			 stmt->domainname);
	}

	/*
	 * Get the information about old types
	 */
	pg_type_rel = heap_openr(TypeRelationName, RowExclusiveLock);
	pg_type_dsc = RelationGetDescr(pg_type_rel);


	/*
	 * When the type is an array for some reason we don't actually receive
	 * the name here.  We receive the base types name.  Lets set Dims while
	 * were at it.
	 */
	if (stmt->typename->arrayBounds > 0) {
		typeName = makeArrayTypeName(stmt->typename->name);

		typNDims = length(stmt->typename->arrayBounds);
	}


	typeTup = SearchSysCache( TYPENAME
							, PointerGetDatum(typeName)
							, 0, 0, 0
							);

	if (!HeapTupleIsValid(typeTup))
	{
		elog(ERROR, "CREATE DOMAIN: type %s does not exist",
			 stmt->typename->name);
	}


	/* Check that this is a basetype */
	typtype = DatumGetChar(heap_getattr(typeTup, Anum_pg_type_typtype, pg_type_dsc, &isnull));
	Assert(!isnull);

	/*
	 * What we really don't want is domains of domains.  This could cause all sorts
	 * of neat issues if we allow that.
	 *
	 * With testing, we may determine complex types should be allowed
	 */
	if (typtype != 'b') {
		elog(ERROR, "DefineDomain: %s is not a basetype", stmt->typename->name);
	}

	/* passed by value */
	byValue = ((Form_pg_type) GETSTRUCT(typeTup))->typbyval;

	/* Required Alignment */
	alignment = ((Form_pg_type) GETSTRUCT(typeTup))->typalign;

	/* Storage Length */
	internalLength = ((Form_pg_type) GETSTRUCT(typeTup))->typlen;

	/* External Length (unused) */
	externalLength = ((Form_pg_type) GETSTRUCT(typeTup))->typprtlen;

	/* Array element Delimiter */
	delimiter = ((Form_pg_type) GETSTRUCT(typeTup))->typdelim;

	/* Input Function Name */
	datum = heap_getattr(typeTup, Anum_pg_type_typinput, pg_type_dsc, &isnull);
	Assert(!isnull);

	inputName = DatumGetCString(DirectFunctionCall1(regprocout, datum));

	/* Output Function Name */
	datum = heap_getattr(typeTup, Anum_pg_type_typoutput, pg_type_dsc, &isnull);
	Assert(!isnull);

	outputName = DatumGetCString(DirectFunctionCall1(regprocout, datum));

	/* ReceiveName */
	datum = heap_getattr(typeTup, Anum_pg_type_typreceive, pg_type_dsc, &isnull);
	Assert(!isnull);

	receiveName = DatumGetCString(DirectFunctionCall1(regprocout, datum));

	/* SendName */
	datum = heap_getattr(typeTup, Anum_pg_type_typsend, pg_type_dsc, &isnull);
	Assert(!isnull);

	sendName = DatumGetCString(DirectFunctionCall1(regprocout, datum));

	/* TOAST Strategy */
	storage =  ((Form_pg_type) GETSTRUCT(typeTup))->typstorage;
	Assert(!isnull);

	/* Inherited default value */
	datum = 			heap_getattr(typeTup, Anum_pg_type_typdefault, pg_type_dsc, &isnull);
	if (!isnull) {
		defaultValue = 	DatumGetCString(DirectFunctionCall1(textout, datum));
	}

	/* Inherited default binary value */
	datum = 			heap_getattr(typeTup, Anum_pg_type_typdefaultbin, pg_type_dsc, &isnull);
	if (!isnull) {
		defaultValueBin = 	DatumGetCString(DirectFunctionCall1(textout, datum));
	}

	/*
	 * Pull out the typelem name of the parent OID.
	 *
	 * This is what enables us to make a domain of an array
	 */
	datum = 			heap_getattr(typeTup, Anum_pg_type_typelem, pg_type_dsc, &isnull);
	Assert(!isnull);

	if (DatumGetObjectId(datum) != InvalidOid) {
		HeapTuple tup;

		tup = SearchSysCache( TYPEOID
							, datum
							, 0, 0, 0
							);

		elemName = NameStr(((Form_pg_type) GETSTRUCT(tup))->typname);

		ReleaseSysCache(tup);
	}


	/*
	 * Run through constraints manually avoids the additional
	 * processing conducted by DefineRelation() and friends.
	 *
	 * Besides, we don't want any constraints to be cooked.  We'll
	 * do that when the table is created via MergeDomainAttributes().
	 */
	foreach(listptr, schema)
	{
		bool nullDefined = false;
		Node	   *expr;
		Constraint *colDef = lfirst(listptr);

		/* Used for the statement transformation */
		ParseState *pstate;

		/*
		 * Create a dummy ParseState and insert the target relation as its
		 * sole rangetable entry.  We need a ParseState for transformExpr.
		 */
		pstate = make_parsestate(NULL);

		switch(colDef->contype) {
			/*
	 		 * The inherited default value may be overridden by the user
			 * with the DEFAULT <expr> statement.
			 *
	 		 * We have to search the entire constraint tree returned as we
			 * don't want to cook or fiddle too much.
			 */
			case CONSTR_DEFAULT:

				/*
				 * Cook the colDef->raw_expr into an expression to ensure
				 * that it can be done.  We store the text version of the
				 * raw value.
				 *
				 * Note: Name is strictly for error message
				 */
				expr = cookDefault(pstate, colDef->raw_expr
								, typeTup->t_data->t_oid
								, stmt->typename->typmod
								, stmt->typename->name);

				/* Binary default required */
				defaultValue = deparse_expression(expr,
								deparse_context_for(stmt->domainname,
													InvalidOid),
												   false);

				defaultValueBin = nodeToString(expr);

				break;

			/*
			 * Find the NULL constraint.
			 */
			case CONSTR_NOTNULL:
				if (nullDefined) {
					elog(ERROR, "CREATE DOMAIN has conflicting NULL / NOT NULL constraint");
				} else {
					typNotNull = true;
					nullDefined = true;
				}

		  		break;

			case CONSTR_NULL:
				if (nullDefined) {
					elog(ERROR, "CREATE DOMAIN has conflicting NULL / NOT NULL constraint");
				} else {
					typNotNull = false;
					nullDefined = true;
				}

		  		break;

		  	case CONSTR_UNIQUE:
		  		elog(ERROR, "CREATE DOMAIN / UNIQUE indecies not supported");
		  		break;

		  	case CONSTR_PRIMARY:
		  		elog(ERROR, "CREATE DOMAIN / PRIMARY KEY indecies not supported");
		  		break;


		  	case CONSTR_CHECK:

		  		elog(ERROR, "defineDomain: CHECK Constraints not supported");
		  		break;

		  	case CONSTR_ATTR_DEFERRABLE:
		  	case CONSTR_ATTR_NOT_DEFERRABLE:
		  	case CONSTR_ATTR_DEFERRED:
		  	case CONSTR_ATTR_IMMEDIATE:
		  		elog(ERROR, "defineDomain: DEFERRABLE, NON DEFERRABLE, DEFERRED and IMMEDIATE not supported");
		  		break;
		}

	}

	/*
	 * Have TypeCreate do all the real work.
	 */
	TypeCreate(stmt->domainname,	/* type name */
			   InvalidOid,			/* preassigned type oid (not done here) */
			   InvalidOid,			/* relation oid (n/a here) */
			   internalLength,		/* internal size */
			   externalLength,		/* external size */
			   'd',					/* type-type (domain type) */
			   delimiter,			/* array element delimiter */
			   inputName,			/* input procedure */
			   outputName,			/* output procedure */
			   receiveName,			/* receive procedure */
			   sendName,			/* send procedure */
			   elemName,			/* element type name */
			   typeName,			/* base type name */
			   defaultValue,		/* default type value */
			   defaultValueBin,		/* default type value */
			   byValue,				/* passed by value */
			   alignment,			/* required alignment */
			   storage,				/* TOAST strategy */
			   stmt->typename->typmod, /* typeMod value */
			   typNDims,			/* Array dimensions for base type */
			   typNotNull);	/* Type NOT NULL */

	/*
	 * Now we can clean up.
	 */
	ReleaseSysCache(typeTup);
	heap_close(pg_type_rel, NoLock);
}


/*
 * DefineType
 *		Registers a new type.
 */
void
DefineType(char *typeName, List *parameters)
{
	int16		internalLength = -1;	/* int2 */
	int16		externalLength = -1;	/* int2 */
	char	   *elemName = NULL;
	char	   *inputName = NULL;
	char	   *outputName = NULL;
	char	   *sendName = NULL;
	char	   *receiveName = NULL;
	char	   *defaultValue = NULL;
	char	   *defaultValueBin = NULL;
	Node	   *defaultRaw = (Node *) NULL;
	bool		byValue = false;
	char		delimiter = DEFAULT_TYPDELIM;
	char	   *shadow_type;
	List	   *pl;
	char		alignment = 'i';	/* default alignment */
	char		storage = 'p';	/* default TOAST storage method */

	/*
	 * Type names must be one character shorter than other names, allowing
	 * room to create the corresponding array type name with prepended
	 * "_".
	 */
	if (strlen(typeName) > (NAMEDATALEN - 2))
		elog(ERROR, "DefineType: type names must be %d characters or less",
			 NAMEDATALEN - 2);

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcasecmp(defel->defname, "internallength") == 0)
			internalLength = defGetTypeLength(defel);
		else if (strcasecmp(defel->defname, "externallength") == 0)
			externalLength = defGetTypeLength(defel);
		else if (strcasecmp(defel->defname, "input") == 0)
			inputName = defGetString(defel);
		else if (strcasecmp(defel->defname, "output") == 0)
			outputName = defGetString(defel);
		else if (strcasecmp(defel->defname, "send") == 0)
			sendName = defGetString(defel);
		else if (strcasecmp(defel->defname, "delimiter") == 0)
		{
			char	   *p = defGetString(defel);

			delimiter = p[0];
		}
		else if (strcasecmp(defel->defname, "receive") == 0)
			receiveName = defGetString(defel);
		else if (strcasecmp(defel->defname, "element") == 0)
			elemName = defGetString(defel);
		else if (strcasecmp(defel->defname, "default") == 0)
			defaultRaw = defel->arg;
		else if (strcasecmp(defel->defname, "passedbyvalue") == 0)
			byValue = true;
		else if (strcasecmp(defel->defname, "alignment") == 0)
		{
			char	   *a = defGetString(defel);

			/*
			 * Note: if argument was an unquoted identifier, parser will
			 * have applied xlateSqlType() to it, so be prepared to
			 * recognize translated type names as well as the nominal
			 * form.
			 */
			if (strcasecmp(a, "double") == 0)
				alignment = 'd';
			else if (strcasecmp(a, "float8") == 0)
				alignment = 'd';
			else if (strcasecmp(a, "int4") == 0)
				alignment = 'i';
			else if (strcasecmp(a, "int2") == 0)
				alignment = 's';
			else if (strcasecmp(a, "char") == 0)
				alignment = 'c';
			else if (strcasecmp(a, "bpchar") == 0)
				alignment = 'c';
			else
				elog(ERROR, "DefineType: \"%s\" alignment not recognized",
					 a);
		}
		else if (strcasecmp(defel->defname, "storage") == 0)
		{
			char	   *a = defGetString(defel);

			if (strcasecmp(a, "plain") == 0)
				storage = 'p';
			else if (strcasecmp(a, "external") == 0)
				storage = 'e';
			else if (strcasecmp(a, "extended") == 0)
				storage = 'x';
			else if (strcasecmp(a, "main") == 0)
				storage = 'm';
			else
				elog(ERROR, "DefineType: \"%s\" storage not recognized",
					 a);
		}
		else
		{
			elog(WARNING, "DefineType: attribute \"%s\" not recognized",
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


	if (defaultRaw) {
		Node   *expr;
		ParseState *pstate;

		/*
		 * Create a dummy ParseState and insert the target relation as its
		 * sole rangetable entry.  We need a ParseState for transformExpr.
		 */
		pstate = make_parsestate(NULL);

		expr = cookDefault(pstate, defaultRaw,
						   InvalidOid,
						   -1,
						   typeName);

		/* Binary default required */
		defaultValue = deparse_expression(expr,
						deparse_context_for(typeName,
											InvalidOid),
										   false);

		defaultValueBin = nodeToString(expr);
	}


	/*
	 * now have TypeCreate do all the real work.
	 */
	TypeCreate(typeName,		/* type name */
			   InvalidOid,		/* preassigned type oid (not done here) */
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
			   NULL,			/* base type name (Non-zero for domains) */
			   defaultValue,	/* default type value */
			   defaultValueBin,	/* default type value (Binary form) */
			   byValue,			/* passed by value */
			   alignment,		/* required alignment */
			   storage,			/* TOAST strategy */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array Dimensions of typbasetype */
			   'f');			/* Type NOT NULL */

	/*
	 * When we create a base type (as opposed to a complex type) we need
	 * to have an array entry for it in pg_type as well.
	 */
	shadow_type = makeArrayTypeName(typeName);

	/* alignment must be 'i' or 'd' for arrays */
	alignment = (alignment == 'd') ? 'd' : 'i';

	TypeCreate(shadow_type,		/* type name */
			   InvalidOid,		/* preassigned type oid (not done here) */
			   InvalidOid,		/* relation oid (n/a here) */
			   -1,				/* internal size */
			   -1,				/* external size */
			   'b',				/* type-type (base type) */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   "array_in",		/* input procedure */
			   "array_out",		/* output procedure */
			   "array_in",		/* receive procedure */
			   "array_out",		/* send procedure */
			   typeName,		/* element type name */
			   NULL,			/* base type name */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* see above */
			   'x',				/* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   'f');			/* Type NOT NULL */

	pfree(shadow_type);
}

static char *
defGetString(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a parameter",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			{
				char	   *str = palloc(32);

				snprintf(str, 32, "%ld", (long) intVal(def->arg));
				return str;
			}
		case T_Float:

			/*
			 * T_Float values are kept in string form, so this type cheat
			 * works (and doesn't risk losing precision)
			 */
			return strVal(def->arg);
		case T_String:
			return strVal(def->arg);
		case T_TypeName:
			return TypeNameToInternalName((TypeName *) def->arg);
		default:
			elog(ERROR, "Define: cannot interpret argument of \"%s\"",
				 def->defname);
	}
	return NULL;				/* keep compiler quiet */
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
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a parameter",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return intVal(def->arg);
		case T_Float:
			elog(ERROR, "Define: \"%s\" requires an integral value",
				 def->defname);
			break;
		case T_String:
			if (strcasecmp(strVal(def->arg), "variable") == 0)
				return -1;		/* variable length */
			break;
		case T_TypeName:
			/* cope if grammar chooses to believe "variable" is a typename */
			if (strcasecmp(TypeNameToInternalName((TypeName *) def->arg),
						   "variable") == 0)
				return -1;		/* variable length */
			break;
		default:
			elog(ERROR, "Define: cannot interpret argument of \"%s\"",
				 def->defname);
	}
	elog(ERROR, "Define: invalid argument for \"%s\"",
		 def->defname);
	return 0;					/* keep compiler quiet */
}
