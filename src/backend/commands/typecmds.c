/*-------------------------------------------------------------------------
 *
 * typecmds.c
 *	  Routines for SQL commands that manipulate types (and domains).
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/typecmds.c,v 1.24 2002/12/13 19:45:52 tgl Exp $
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
 *				input/output functions
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/var.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

static Oid	findTypeIOFunction(List *procname, Oid typeOid, bool isOutput);
static List *get_rels_with_domain(Oid domainOid);
static void domainPermissionCheck(HeapTuple tup, TypeName *typename);
static char *domainAddConstraint(Oid domainOid, Oid domainNamespace,
								 Oid baseTypeOid,
								 int typMod, Constraint *constr,
								 int *counter, char *domainName);

typedef struct
{
	Oid		relOid;
	int		natts;
	int		*atts;
} relToCheck;

/*
 * DefineType
 *		Registers a new type.
 */
void
DefineType(List *names, List *parameters)
{
	char	   *typeName;
	Oid			typeNamespace;
	AclResult	aclresult;
	int16		internalLength = -1;	/* int2 */
	Oid			elemType = InvalidOid;
	List	   *inputName = NIL;
	List	   *outputName = NIL;
	char	   *defaultValue = NULL;
	bool		byValue = false;
	char		delimiter = DEFAULT_TYPDELIM;
	char		alignment = 'i';	/* default alignment */
	char		storage = 'p';	/* default TOAST storage method */
	Oid			inputOid;
	Oid			outputOid;
	char	   *shadow_type;
	List	   *pl;
	Oid			typoid;
	Oid			resulttype;

	/* Convert list of names to a name and namespace */
	typeNamespace = QualifiedNameGetCreationNamespace(names, &typeName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(typeNamespace));

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
			;					/* ignored -- remove after 7.3 */
		else if (strcasecmp(defel->defname, "input") == 0)
			inputName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "output") == 0)
			outputName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "send") == 0)
			;					/* ignored -- remove after 7.3 */
		else if (strcasecmp(defel->defname, "receive") == 0)
			;					/* ignored -- remove after 7.3 */
		else if (strcasecmp(defel->defname, "delimiter") == 0)
		{
			char	   *p = defGetString(defel);

			delimiter = p[0];
		}
		else if (strcasecmp(defel->defname, "element") == 0)
		{
			elemType = typenameTypeId(defGetTypeName(defel));
			/* disallow arrays of pseudotypes */
			if (get_typtype(elemType) == 'p')
				elog(ERROR, "Array element type cannot be %s",
					 format_type_be(elemType));
		}
		else if (strcasecmp(defel->defname, "default") == 0)
			defaultValue = defGetString(defel);
		else if (strcasecmp(defel->defname, "passedbyvalue") == 0)
			byValue = true;
		else if (strcasecmp(defel->defname, "alignment") == 0)
		{
			char	   *a = defGetString(defel);

			/*
			 * Note: if argument was an unquoted identifier, parser will
			 * have applied translations to it, so be prepared to
			 * recognize translated type names as well as the nominal
			 * form.
			 */
			if (strcasecmp(a, "double") == 0 ||
				strcasecmp(a, "float8") == 0 ||
				strcasecmp(a, "pg_catalog.float8") == 0)
				alignment = 'd';
			else if (strcasecmp(a, "int4") == 0 ||
					 strcasecmp(a, "pg_catalog.int4") == 0)
				alignment = 'i';
			else if (strcasecmp(a, "int2") == 0 ||
					 strcasecmp(a, "pg_catalog.int2") == 0)
				alignment = 's';
			else if (strcasecmp(a, "char") == 0 ||
					 strcasecmp(a, "pg_catalog.bpchar") == 0)
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
	if (inputName == NIL)
		elog(ERROR, "Define: \"input\" unspecified");
	if (outputName == NIL)
		elog(ERROR, "Define: \"output\" unspecified");

	/*
	 * Look to see if type already exists (presumably as a shell; if not,
	 * TypeCreate will complain).  If it doesn't, create it as a shell,
	 * so that the OID is known for use in the I/O function definitions.
	 */
	typoid = GetSysCacheOid(TYPENAMENSP,
							CStringGetDatum(typeName),
							ObjectIdGetDatum(typeNamespace),
							0, 0);
	if (!OidIsValid(typoid))
	{
		typoid = TypeShellMake(typeName, typeNamespace);
		/* Make new shell type visible for modification below */
		CommandCounterIncrement();
	}

	/*
	 * Convert I/O proc names to OIDs
	 */
	inputOid = findTypeIOFunction(inputName, typoid, false);
	outputOid = findTypeIOFunction(outputName, typoid, true);

	/*
	 * Verify that I/O procs return the expected thing.  If we see OPAQUE,
	 * complain and change it to the correct type-safe choice.
	 */
	resulttype = get_func_rettype(inputOid);
	if (resulttype != typoid)
	{
		if (resulttype == OPAQUEOID)
		{
			elog(NOTICE, "TypeCreate: changing return type of function %s from OPAQUE to %s",
				 NameListToString(inputName), typeName);
			SetFunctionReturnType(inputOid, typoid);
		}
		else
			elog(ERROR, "Type input function %s must return %s",
				 NameListToString(inputName), typeName);
	}
	resulttype = get_func_rettype(outputOid);
	if (resulttype != CSTRINGOID)
	{
		if (resulttype == OPAQUEOID)
		{
			elog(NOTICE, "TypeCreate: changing return type of function %s from OPAQUE to CSTRING",
				 NameListToString(outputName));
			SetFunctionReturnType(outputOid, CSTRINGOID);
		}
		else
			elog(ERROR, "Type output function %s must return cstring",
				 NameListToString(outputName));
	}

	/*
	 * now have TypeCreate do all the real work.
	 */
	typoid =
		TypeCreate(typeName,	/* type name */
				   typeNamespace,		/* namespace */
				   InvalidOid,	/* preassigned type oid (not done here) */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   internalLength,		/* internal size */
				   'b',			/* type-type (base type) */
				   delimiter,	/* array element delimiter */
				   inputOid,	/* input procedure */
				   outputOid,	/* output procedure */
				   elemType,	/* element type ID */
				   InvalidOid,	/* base type ID (only for domains) */
				   defaultValue,	/* default type value */
				   NULL,		/* no binary form available */
				   byValue,		/* passed by value */
				   alignment,	/* required alignment */
				   storage,		/* TOAST strategy */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array Dimensions of typbasetype */
				   false);		/* Type NOT NULL */

	/*
	 * When we create a base type (as opposed to a complex type) we need
	 * to have an array entry for it in pg_type as well.
	 */
	shadow_type = makeArrayTypeName(typeName);

	/* alignment must be 'i' or 'd' for arrays */
	alignment = (alignment == 'd') ? 'd' : 'i';

	TypeCreate(shadow_type,		/* type name */
			   typeNamespace,	/* namespace */
			   InvalidOid,		/* preassigned type oid (not done here) */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   -1,				/* internal size */
			   'b',				/* type-type (base type) */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   typoid,			/* element type ID */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* see above */
			   'x',				/* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false);			/* Type NOT NULL */

	pfree(shadow_type);
}


/*
 *	RemoveType
 *		Removes a datatype.
 */
void
RemoveType(List *names, DropBehavior behavior)
{
	TypeName   *typename;
	Oid			typeoid;
	HeapTuple	tup;
	ObjectAddress object;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	/* Use LookupTypeName here so that shell types can be removed. */
	typeoid = LookupTypeName(typename);
	if (!OidIsValid(typeoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typeoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	/* Permission check: must own type or its namespace */
	if (!pg_type_ownercheck(typeoid, GetUserId()) &&
		!pg_namespace_ownercheck(((Form_pg_type) GETSTRUCT(tup))->typnamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, TypeNameToString(typename));

	ReleaseSysCache(tup);

	/*
	 * Do the deletion
	 */
	object.classId = RelOid_pg_type;
	object.objectId = typeoid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}


/*
 * Guts of type deletion.
 */
void
RemoveTypeById(Oid typeOid)
{
	Relation	relation;
	HeapTuple	tup;

	relation = heap_openr(TypeRelationName, RowExclusiveLock);

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typeOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "RemoveTypeById: type %u not found",
			 typeOid);

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}


/*
 * DefineDomain
 *		Registers a new domain.
 */
void
DefineDomain(CreateDomainStmt *stmt)
{
	char	   *domainName;
	Oid			domainNamespace;
	AclResult	aclresult;
	int16		internalLength;
	Oid			inputProcedure;
	Oid			outputProcedure;
	bool		byValue;
	char		delimiter;
	char		alignment;
	char		storage;
	char		typtype;
	Datum		datum;
	bool		isnull;
	Node	   *defaultExpr = NULL;
	char	   *defaultValue = NULL;
	char	   *defaultValueBin = NULL;
	bool		typNotNull = false;
	bool		nullDefined = false;
	Oid			basetypelem;
	int32		typNDims = length(stmt->typename->arrayBounds);
	HeapTuple	typeTup;
	List	   *schema = stmt->constraints;
	List	   *listptr;
	Oid			basetypeoid;
	Oid			domainoid;
	Form_pg_type	baseType;
	int			counter = 0;

	/* Convert list of names to a name and namespace */
	domainNamespace = QualifiedNameGetCreationNamespace(stmt->domainname,
														&domainName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(domainNamespace, GetUserId(),
									  ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_namespace_name(domainNamespace));

	/*
	 * Domainnames, unlike typenames don't need to account for the '_'
	 * prefix.	So they can be one character longer.
	 */
	if (strlen(domainName) > (NAMEDATALEN - 1))
		elog(ERROR, "CREATE DOMAIN: domain names must be %d characters or less",
			 NAMEDATALEN - 1);

	/*
	 * Look up the base type.
	 */
	typeTup = typenameType(stmt->typename);

	baseType = (Form_pg_type) GETSTRUCT(typeTup);
	basetypeoid = HeapTupleGetOid(typeTup);

	/*
	 * Base type must be a plain base type.  Domains over pseudo types
	 * would create a security hole.  Domains of domains might be made to
	 * work in the future, but not today.  Ditto for domains over complex
	 * types.
	 */
	typtype = baseType->typtype;
	if (typtype != 'b')
		elog(ERROR, "DefineDomain: %s is not a basetype",
			 TypeNameToString(stmt->typename));

	/* passed by value */
	byValue = baseType->typbyval;

	/* Required Alignment */
	alignment = baseType->typalign;

	/* TOAST Strategy */
	storage = baseType->typstorage;

	/* Storage Length */
	internalLength = baseType->typlen;

	/* Array element Delimiter */
	delimiter = baseType->typdelim;

	/* I/O Functions */
	inputProcedure = baseType->typinput;
	outputProcedure = baseType->typoutput;

	/* Inherited default value */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefault, &isnull);
	if (!isnull)
		defaultValue = DatumGetCString(DirectFunctionCall1(textout, datum));

	/* Inherited default binary value */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefaultbin, &isnull);
	if (!isnull)
		defaultValueBin = DatumGetCString(DirectFunctionCall1(textout, datum));

	/*
	 * Pull out the typelem name of the parent OID.
	 *
	 * This is what enables us to make a domain of an array
	 */
	basetypelem = baseType->typelem;

	/*
	 * Run through constraints manually to avoid the additional
	 * processing conducted by DefineRelation() and friends.
	 */
	foreach(listptr, schema)
	{
		Node	   *newConstraint = lfirst(listptr);
		Constraint *colDef;
		ParseState *pstate;

		/* Check for unsupported constraint types */
		if (IsA(newConstraint, FkConstraint))
			elog(ERROR, "CREATE DOMAIN / FOREIGN KEY constraints not supported");

		/* this case should not happen */
		if (!IsA(newConstraint, Constraint))
			elog(ERROR, "DefineDomain: unexpected constraint node type");

		colDef = (Constraint *) newConstraint;

		switch (colDef->contype)
		{
			case CONSTR_DEFAULT:
				/*
				 * The inherited default value may be overridden by the
				 * user with the DEFAULT <expr> statement.
				 */
				if (defaultExpr)
					elog(ERROR, "CREATE DOMAIN has multiple DEFAULT expressions");
				/* Create a dummy ParseState for transformExpr */
				pstate = make_parsestate(NULL);

				/*
				 * Cook the colDef->raw_expr into an expression. Note:
				 * Name is strictly for error message
				 */
				defaultExpr = cookDefault(pstate, colDef->raw_expr,
										  basetypeoid,
										  stmt->typename->typmod,
										  domainName);

				/*
				 * Expression must be stored as a nodeToString result, but
				 * we also require a valid textual representation (mainly
				 * to make life easier for pg_dump).
				 */
				defaultValue = deparse_expression(defaultExpr,
										  deparse_context_for(domainName,
															  InvalidOid),
												  false, false);
				defaultValueBin = nodeToString(defaultExpr);
				break;

			case CONSTR_NOTNULL:
				if (nullDefined && !typNotNull)
					elog(ERROR, "CREATE DOMAIN has conflicting NULL / NOT NULL constraint");
				typNotNull = true;
				nullDefined = true;
				break;

			case CONSTR_NULL:
				if (nullDefined && typNotNull)
					elog(ERROR, "CREATE DOMAIN has conflicting NULL / NOT NULL constraint");
				typNotNull = false;
				nullDefined = true;
		  		break;

		  	case CONSTR_CHECK:
				/*
				 * Check constraints are handled after domain creation, as they
				 * require the Oid of the domain
				 */
		  		break;

				/*
				 * All else are error cases
				 */
		  	case CONSTR_UNIQUE:
		  		elog(ERROR, "CREATE DOMAIN / UNIQUE not supported");
		  		break;

		  	case CONSTR_PRIMARY:
		  		elog(ERROR, "CREATE DOMAIN / PRIMARY KEY not supported");
		  		break;

		  	case CONSTR_ATTR_DEFERRABLE:
		  	case CONSTR_ATTR_NOT_DEFERRABLE:
		  	case CONSTR_ATTR_DEFERRED:
		  	case CONSTR_ATTR_IMMEDIATE:
		  		elog(ERROR, "CREATE DOMAIN: DEFERRABLE, NON DEFERRABLE, DEFERRED"
							" and IMMEDIATE not supported");
		  		break;

			default:
				elog(ERROR, "DefineDomain: unrecognized constraint subtype");
				break;
		}
	}

	/*
	 * Have TypeCreate do all the real work.
	 */
	domainoid =
		TypeCreate(domainName,	/* type name */
				   domainNamespace,		/* namespace */
				   InvalidOid,	/* preassigned type oid (none here) */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   internalLength,		/* internal size */
				   'd',			/* type-type (domain type) */
				   delimiter,	/* array element delimiter */
				   inputProcedure,		/* input procedure */
				   outputProcedure,		/* output procedure */
				   basetypelem, /* element type ID */
				   basetypeoid, /* base type ID */
				   defaultValue,	/* default type value (text) */
				   defaultValueBin,		/* default type value (binary) */
				   byValue,				/* passed by value */
				   alignment,			/* required alignment */
				   storage,				/* TOAST strategy */
				   stmt->typename->typmod, /* typeMod value */
				   typNDims,			/* Array dimensions for base type */
				   typNotNull);			/* Type NOT NULL */

	/*
	 * Process constraints which refer to the domain ID returned by TypeCreate
	 */
	foreach(listptr, schema)
	{
		Constraint *constr = lfirst(listptr);

		/* it must be a Constraint, per check above */

		switch (constr->contype)
		{
		  	case CONSTR_CHECK:
				domainAddConstraint(domainoid, domainNamespace,
									basetypeoid, stmt->typename->typmod,
									constr, &counter, domainName);
		  		break;

			/* Other constraint types were fully processed above */

			default:
		  		break;
		}
	}

	/*
	 * Now we can clean up.
	 */
	ReleaseSysCache(typeTup);
}


/*
 *	RemoveDomain
 *		Removes a domain.
 *
 * This is identical to RemoveType except we insist it be a domain.
 */
void
RemoveDomain(List *names, DropBehavior behavior)
{
	TypeName   *typename;
	Oid			typeoid;
	HeapTuple	tup;
	char		typtype;
	ObjectAddress object;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	/* Use LookupTypeName here so that shell types can be removed. */
	typeoid = LookupTypeName(typename);
	if (!OidIsValid(typeoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCache(TYPEOID,
						 ObjectIdGetDatum(typeoid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "RemoveDomain: type \"%s\" does not exist",
			 TypeNameToString(typename));

	/* Permission check: must own type or its namespace */
	if (!pg_type_ownercheck(typeoid, GetUserId()) &&
		!pg_namespace_ownercheck(((Form_pg_type) GETSTRUCT(tup))->typnamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, TypeNameToString(typename));

	/* Check that this is actually a domain */
	typtype = ((Form_pg_type) GETSTRUCT(tup))->typtype;

	if (typtype != 'd')
		elog(ERROR, "%s is not a domain",
			 TypeNameToString(typename));

	ReleaseSysCache(tup);

	/*
	 * Do the deletion
	 */
	object.classId = RelOid_pg_type;
	object.objectId = typeoid;
	object.objectSubId = 0;

	performDeletion(&object, behavior);
}


/*
 * Find a suitable I/O function for a type.
 *
 * typeOid is the type's OID (which will already exist, if only as a shell
 * type).
 */
static Oid
findTypeIOFunction(List *procname, Oid typeOid, bool isOutput)
{
	Oid			argList[FUNC_MAX_ARGS];
	Oid			procOid;

	if (isOutput)
	{
		/*
		 * Output functions can take a single argument of the type, or two
		 * arguments (data value, element OID).
		 *
		 * For backwards compatibility we allow OPAQUE in place of the actual
		 * type name; if we see this, we issue a NOTICE and fix up the
		 * pg_proc entry.
		 */
		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		argList[0] = typeOid;

		procOid = LookupFuncName(procname, 1, argList);
		if (OidIsValid(procOid))
			return procOid;

		argList[1] = OIDOID;

		procOid = LookupFuncName(procname, 2, argList);
		if (OidIsValid(procOid))
			return procOid;

		/* No luck, try it with OPAQUE */
		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		argList[0] = OPAQUEOID;

		procOid = LookupFuncName(procname, 1, argList);

		if (!OidIsValid(procOid))
		{
			argList[1] = OIDOID;

			procOid = LookupFuncName(procname, 2, argList);
		}

		if (OidIsValid(procOid))
		{
			/* Found, but must complain and fix the pg_proc entry */
			elog(NOTICE, "TypeCreate: changing argument type of function %s from OPAQUE to %s",
				 NameListToString(procname), format_type_be(typeOid));
			SetFunctionArgType(procOid, 0, typeOid);
			/*
			 * Need CommandCounterIncrement since DefineType will likely
			 * try to alter the pg_proc tuple again.
			 */
			CommandCounterIncrement();

			return procOid;
		}

		/* Use type name, not OPAQUE, in the failure message. */
		argList[0] = typeOid;

		func_error("TypeCreate", procname, 1, argList, NULL);
	}
	else
	{
		/*
		 * Input functions can take a single argument of type CSTRING, or
		 * three arguments (string, element OID, typmod).
		 *
		 * For backwards compatibility we allow OPAQUE in place of CSTRING;
		 * if we see this, we issue a NOTICE and fix up the pg_proc entry.
		 */
		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		argList[0] = CSTRINGOID;

		procOid = LookupFuncName(procname, 1, argList);
		if (OidIsValid(procOid))
			return procOid;

		argList[1] = OIDOID;
		argList[2] = INT4OID;

		procOid = LookupFuncName(procname, 3, argList);
		if (OidIsValid(procOid))
			return procOid;

		/* No luck, try it with OPAQUE */
		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		argList[0] = OPAQUEOID;

		procOid = LookupFuncName(procname, 1, argList);

		if (!OidIsValid(procOid))
		{
			argList[1] = OIDOID;
			argList[2] = INT4OID;

			procOid = LookupFuncName(procname, 3, argList);
		}

		if (OidIsValid(procOid))
		{
			/* Found, but must complain and fix the pg_proc entry */
			elog(NOTICE, "TypeCreate: changing argument type of function %s "
				 "from OPAQUE to CSTRING",
				 NameListToString(procname));
			SetFunctionArgType(procOid, 0, CSTRINGOID);
			/*
			 * Need CommandCounterIncrement since DefineType will likely
			 * try to alter the pg_proc tuple again.
			 */
			CommandCounterIncrement();

			return procOid;
		}

		/* Use CSTRING (preferred) in the error message */
		argList[0] = CSTRINGOID;

		func_error("TypeCreate", procname, 1, argList, NULL);
	}

	return InvalidOid;			/* keep compiler quiet */
}


/*-------------------------------------------------------------------
 * DefineCompositeType
 *
 * Create a Composite Type relation.
 * `DefineRelation' does all the work, we just provide the correct
 * arguments!
 *
 * If the relation already exists, then 'DefineRelation' will abort
 * the xact...
 *
 * DefineCompositeType returns relid for use when creating
 * an implicit composite type during function creation
 *-------------------------------------------------------------------
 */
Oid
DefineCompositeType(const RangeVar *typevar, List *coldeflist)
{
	CreateStmt *createStmt = makeNode(CreateStmt);

	if (coldeflist == NIL)
		elog(ERROR, "attempted to define composite type relation with"
			 " no attrs");

	/*
	 * now create the parameters for keys/inheritance etc. All of them are
	 * nil...
	 */
	createStmt->relation = (RangeVar *) typevar;
	createStmt->tableElts = coldeflist;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->hasoids = false;
	createStmt->oncommit = ONCOMMIT_NOOP;

	/*
	 * finally create the relation...
	 */
	return DefineRelation(createStmt, RELKIND_COMPOSITE_TYPE);
}

/*
 * AlterDomainDefault
 *
 * Routine implementing ALTER DOMAIN SET/DROP DEFAULT statements. 
 */
void
AlterDomainDefault(List *names, Node *defaultRaw)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	ParseState *pstate;
	Relation	rel;
	char	   *defaultValue;
	Node	   *defaultExpr = NULL; /* NULL if no default specified */
	Datum		new_record[Natts_pg_type];
	char		new_record_nulls[Natts_pg_type];
	char		new_record_repl[Natts_pg_type];
	HeapTuple	newtuple;
	Form_pg_type	typTup;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	/* Lock the domain in the type table */
	rel = heap_openr(TypeRelationName, RowExclusiveLock);

	/* Use LookupTypeName here so that shell types can be removed. */
	domainoid = LookupTypeName(typename);
	if (!OidIsValid(domainoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCacheCopy(TYPEOID,
							 ObjectIdGetDatum(domainoid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AlterDomain: type \"%s\" does not exist",
			 TypeNameToString(typename));

	/* Doesn't return if user isn't allowed to alter the domain */ 
	domainPermissionCheck(tup, typename);

	/* Setup new tuple */
	MemSet(new_record, (Datum) 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	/* Useful later */
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Store the new default, if null then skip this step */
	if (defaultRaw)
	{
		/* Create a dummy ParseState for transformExpr */
		pstate = make_parsestate(NULL);
		/*
		 * Cook the colDef->raw_expr into an expression. Note:
		 * Name is strictly for error message
		 */
		defaultExpr = cookDefault(pstate, defaultRaw,
								  typTup->typbasetype,
								  typTup->typtypmod,
								  NameStr(typTup->typname));

		/*
		 * Expression must be stored as a nodeToString result, but
		 * we also require a valid textual representation (mainly
		 * to make life easier for pg_dump).
		 */
		defaultValue = deparse_expression(defaultExpr,
								  deparse_context_for(NameStr(typTup->typname),
													  InvalidOid),
										  false, false);
		/*
		 * Form an updated tuple with the new default and write it back.
		 */
		new_record[Anum_pg_type_typdefaultbin - 1] = DirectFunctionCall1(textin,
														CStringGetDatum(
															nodeToString(defaultExpr)));

		new_record_repl[Anum_pg_type_typdefaultbin - 1] = 'r';
		new_record[Anum_pg_type_typdefault - 1] = DirectFunctionCall1(textin,
								   					CStringGetDatum(defaultValue));
		new_record_repl[Anum_pg_type_typdefault - 1] = 'r';
	}
	else /* Default is NULL, drop it */
	{
		new_record_nulls[Anum_pg_type_typdefaultbin - 1] = 'n';
		new_record_repl[Anum_pg_type_typdefaultbin - 1] = 'r';
		new_record_nulls[Anum_pg_type_typdefault - 1] = 'n';
		new_record_repl[Anum_pg_type_typdefault - 1] = 'r';
	}

	newtuple = heap_modifytuple(tup, rel,
								new_record, new_record_nulls, new_record_repl);

	simple_heap_update(rel, &tup->t_self, newtuple);

	CatalogUpdateIndexes(rel, newtuple);

	/* Rebuild dependencies */
	GenerateTypeDependencies(typTup->typnamespace,
							 domainoid,
							 typTup->typrelid,
							 InvalidOid,
							 typTup->typinput,
							 typTup->typoutput,
							 typTup->typelem,
							 typTup->typbasetype,
							 nodeToString(defaultExpr),
							 true); /* Rebuild is true */

	/* Clean up */
	heap_close(rel, NoLock);
	heap_freetuple(newtuple);
};

/*
 * AlterDomainNotNull
 *
 * Routine implementing ALTER DOMAIN SET/DROP NOT NULL statements. 
 */
void
AlterDomainNotNull(List *names, bool notNull)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	Relation	rel;
	Datum		new_record[Natts_pg_type];
	char		new_record_nulls[Natts_pg_type];
	char		new_record_repl[Natts_pg_type];
	HeapTuple	newtuple;
	Form_pg_type	typTup;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	/* Lock the type table */
	rel = heap_openr(TypeRelationName, RowExclusiveLock);

	/* Use LookupTypeName here so that shell types can be removed. */
	domainoid = LookupTypeName(typename);
	if (!OidIsValid(domainoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCacheCopy(TYPEOID,
							 ObjectIdGetDatum(domainoid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AlterDomain: type \"%s\" does not exist",
			 TypeNameToString(typename));

	/* Doesn't return if user isn't allowed to alter the domain */ 
	domainPermissionCheck(tup, typename);

	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Is the domain already set to the destination constraint? */
	if (typTup->typnotnull == notNull)
		elog(ERROR, "AlterDomain: %s is already set to %s",
			 TypeNameToString(typename),
			 notNull ? "NOT NULL" : "NULL");

	/* Adding a NOT NULL constraint requires checking current domains */
	if (notNull)
	{
		List   *rels;
		List   *rt;

		/* Fetch relation list with attributes based on this domain */
		rels = get_rels_with_domain(domainoid);

		foreach (rt, rels)
		{
			Relation	typrel;
			HeapTuple	tuple;
			HeapScanDesc scan;
			TupleDesc	tupdesc;
			relToCheck *rtc = (relToCheck *) lfirst(rt);

			/* Lock relation */
			typrel = heap_open(rtc->relOid, ExclusiveLock);

			tupdesc = RelationGetDescr(typrel);

			/* Fetch tuples sequentially */
			scan = heap_beginscan(typrel, SnapshotNow, 0, NULL);
			while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
			{
				int		i;

				/* Test attributes */
				for (i = 0; i < rtc->natts; i++)
				{
					Datum	d;
					bool	isNull;

					d = heap_getattr(tuple, rtc->atts[i], tupdesc, &isNull);

					if (isNull)
						elog(ERROR, "ALTER DOMAIN: Relation \"%s\" Attribute \"%s\" "
							 "contains NULL values",
							 RelationGetRelationName(typrel),
							 NameStr(*attnumAttName(typrel, rtc->atts[i])));
				}
			}

			heap_endscan(scan);

			/* Release lock */
			heap_close(typrel, NoLock);
		}
	}


	/* Setup new tuple */
	MemSet(new_record, (Datum) 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	new_record[Anum_pg_type_typnotnull - 1] = BoolGetDatum(notNull);
	new_record_repl[Anum_pg_type_typnotnull - 1] = 'r';

	/* Build the new tuple */
	newtuple = heap_modifytuple(tup, rel,
								new_record, new_record_nulls, new_record_repl);

	simple_heap_update(rel, &tup->t_self, newtuple);

	CatalogUpdateIndexes(rel, newtuple);

	/* Clean up */
	heap_close(rel, NoLock);
	heap_freetuple(newtuple);
}

/*
 * AlterDomainDropConstraint
 *
 * Implements the ALTER DOMAIN DROP CONSTRAINT statement
 */
void
AlterDomainDropConstraint(List *names, const char *constrName, DropBehavior behavior)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	Relation	rel;
	Form_pg_type	typTup;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData key[1];
	HeapTuple	contup;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	/* Lock the type table */
	rel = heap_openr(TypeRelationName, RowExclusiveLock);

	/* Use LookupTypeName here so that shell types can be removed. */
	domainoid = LookupTypeName(typename);
	if (!OidIsValid(domainoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCacheCopy(TYPEOID,
							 ObjectIdGetDatum(domainoid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AlterDomain: type \"%s\" does not exist",
			 TypeNameToString(typename));

	/* Doesn't return if user isn't allowed to alter the domain */ 
	domainPermissionCheck(tup, typename);

	/* Grab an appropriate lock on the pg_constraint relation */
	conrel = heap_openr(ConstraintRelationName, RowExclusiveLock);

	/* Use the index to scan only constraints of the target relation */
	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_constraint_contypid, F_OIDEQ,
						   ObjectIdGetDatum(HeapTupleGetOid(tup)));

	conscan = systable_beginscan(conrel, ConstraintTypidIndex, true,
								 SnapshotNow, 1, key);

	typTup = (Form_pg_type) GETSTRUCT(tup);

	/*
	 * Scan over the result set, removing any matching entries.
	 */
	while ((contup = systable_getnext(conscan)) != NULL)
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(contup);

		if (strcmp(NameStr(con->conname), constrName) == 0)
		{
			ObjectAddress conobj;

			conobj.classId = RelationGetRelid(conrel);
			conobj.objectId = HeapTupleGetOid(contup);
			conobj.objectSubId = 0;

			performDeletion(&conobj, behavior);
		}
	}
	/* Clean up after the scan */
	systable_endscan(conscan);
	heap_close(conrel, RowExclusiveLock);

	heap_close(rel, NoLock);
};

/*
 * AlterDomainAddConstraint
 *
 * Implements the ALTER DOMAIN .. ADD CONSTRAINT statement.
 */
void
AlterDomainAddConstraint(List *names, Node *newConstraint)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	Relation	rel;
	List   *rels;
	List   *rt;
	Form_pg_type	typTup;
	ExprContext *econtext;
	char   *ccbin;
	Expr   *expr;
	ExprState *exprstate;
	int		counter = 0;
	Constraint *constr;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeNode(TypeName);
	typename->names = names;
	typename->typmod = -1;
	typename->arrayBounds = NIL;

	/* Lock the type table */
	rel = heap_openr(TypeRelationName, RowExclusiveLock);

	/* Use LookupTypeName here so that shell types can be found. */
	domainoid = LookupTypeName(typename);
	if (!OidIsValid(domainoid))
		elog(ERROR, "Type \"%s\" does not exist",
			 TypeNameToString(typename));

	tup = SearchSysCacheCopy(TYPEOID,
							 ObjectIdGetDatum(domainoid),
							 0, 0, 0);

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "AlterDomain: type \"%s\" does not exist",
			 TypeNameToString(typename));

	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Doesn't return if user isn't allowed to alter the domain */ 
	domainPermissionCheck(tup, typename);

	/* Check for unsupported constraint types */
	if (IsA(newConstraint, FkConstraint))
		elog(ERROR, "ALTER DOMAIN / FOREIGN KEY constraints not supported");

	/* this case should not happen */
	if (!IsA(newConstraint, Constraint))
		elog(ERROR, "AlterDomainAddConstraint: unexpected constraint node type");

	constr = (Constraint *) newConstraint;

	switch (constr->contype)
	{
		case CONSTR_DEFAULT:
			elog(ERROR, "Use ALTER DOMAIN .. SET DEFAULT instead");
			break;

		case CONSTR_NOTNULL:
		case CONSTR_NULL:
			elog(ERROR, "Use ALTER DOMAIN .. [ SET | DROP ] NOT NULL instead");
			break;

	  	case CONSTR_CHECK:
			/* processed below */
	  		break;

		case CONSTR_UNIQUE:
			elog(ERROR, "ALTER DOMAIN / UNIQUE indexes not supported");
			break;

		case CONSTR_PRIMARY:
			elog(ERROR, "ALTER DOMAIN / PRIMARY KEY indexes not supported");
			break;

		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			elog(ERROR, "ALTER DOMAIN: DEFERRABLE, NON DEFERRABLE, DEFERRED"
				 " and IMMEDIATE not supported");
			break;

		default:
			elog(ERROR, "AlterDomainAddConstraint: unrecognized constraint node type");
			break;
	}

	/*
	 * Since all other constraint types throw errors, this must be
	 * a check constraint.  First, process the constraint expression
	 * and add an entry to pg_constraint.
	 */

	ccbin = domainAddConstraint(HeapTupleGetOid(tup), typTup->typnamespace,
								typTup->typbasetype, typTup->typtypmod,
								constr, &counter, NameStr(typTup->typname));

	/*
	 * Test all values stored in the attributes based on the domain
	 * the constraint is being added to.
	 */
	expr = (Expr *) stringToNode(ccbin);
	fix_opfuncids((Node *) expr);
	exprstate = ExecInitExpr(expr, NULL);

	/* Make an expression context for ExecEvalExpr */
	econtext = MakeExprContext(NULL, CurrentMemoryContext);

	rels = get_rels_with_domain(domainoid);

	foreach (rt, rels)
	{
		relToCheck *rtc = (relToCheck *) lfirst(rt);
		Relation	testrel;
		TupleDesc	tupdesc;
		HeapTuple	tuple;
		HeapScanDesc scan;

		/* Lock relation against changes */
		testrel = heap_open(rtc->relOid, ShareLock);

		tupdesc = RelationGetDescr(testrel);

		/* Scan through table */
		scan = heap_beginscan(testrel, SnapshotNow, 0, NULL);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			int		i;

			/* Loop through each attribute of the tuple with a domain */
			for (i = 0; i < rtc->natts; i++)
			{
				Datum	d;
				bool	isNull;
				Datum   conResult;

				d = heap_getattr(tuple, rtc->atts[i], tupdesc, &isNull);

				econtext->domainValue_datum = d;
				econtext->domainValue_isNull = isNull;

				conResult = ExecEvalExpr(exprstate, econtext, &isNull, NULL);

				if (!isNull && !DatumGetBool(conResult))
					elog(ERROR, "AlterDomainAddConstraint: Domain %s constraint %s failed",
						 NameStr(typTup->typname), constr->name);
			}

			ResetExprContext(econtext);
		}

		heap_endscan(scan);

		/* Hold relation lock till commit (XXX bad for concurrency) */
		heap_close(testrel, NoLock);
	}

	FreeExprContext(econtext);

	/* Clean up */
	heap_close(rel, NoLock);
}

/*
 * get_rels_with_domain
 *
 * Fetch all relations / attributes which are using the domain
 * while maintaining a RowExclusiveLock on the pg_attribute
 * entries.
 *
 * Generally used for retrieving a list of tests when adding
 * new constraints to a domain.
 */
List *
get_rels_with_domain(Oid domainOid)
{
	Relation	classRel;
	HeapTuple	classTup;
	Relation	attRel;
	HeapScanDesc	classScan;
	List *rels = NIL;

	/*
	 * We need to lock the domain rows for the length of the transaction,
	 * but once all of the tables and the appropriate attributes are
	 * found we can relese the relation lock.
	 */
	classRel = relation_openr(RelationRelationName, ExclusiveLock);
	attRel = relation_openr(AttributeRelationName, RowExclusiveLock);

	classScan = heap_beginscan(classRel, SnapshotNow, 0, NULL);

	/* Scan through pg_class for tables */
	while ((classTup = heap_getnext(classScan, ForwardScanDirection)) != NULL)
	{
		relToCheck *rtc = NULL;
		int			nkeys = 0;
		HeapTuple	attTup;
		HeapScanDesc	attScan;
		ScanKeyData		attKey[2];
		Form_pg_class	pg_class;

		/* Get our pg_class struct */
		pg_class = (Form_pg_class) GETSTRUCT(classTup);

		/* Fetch attributes from pg_attribute for the relation of the type domainOid */
		ScanKeyEntryInitialize(&attKey[nkeys++], 0, Anum_pg_attribute_attrelid,
						   F_OIDEQ, ObjectIdGetDatum(HeapTupleGetOid(classTup)));

		ScanKeyEntryInitialize(&attKey[nkeys++], 0, Anum_pg_attribute_atttypid,
						   F_OIDEQ, ObjectIdGetDatum(domainOid));

		/* Setup to scan pg_attribute */
		attScan = heap_beginscan(attRel, SnapshotNow, nkeys, attKey);

		/* Scan through pg_attribute for attributes based on the domain */
		while ((attTup = heap_getnext(attScan, ForwardScanDirection)) != NULL)
		{
			if (rtc == NULL)
			{
				/* First one found for this rel */
				rtc = (relToCheck *)palloc(sizeof(relToCheck));
				rtc->atts = (int *)palloc(sizeof(int) * pg_class->relnatts);
				rtc->relOid = HeapTupleGetOid(classTup);
				rtc->natts = 0;
				rels = lcons((void *)rtc, rels);
			}

			/* Now add the attribute */
			rtc->atts[rtc->natts++] = ((Form_pg_attribute) GETSTRUCT(attTup))->attnum;
		}

		heap_endscan(attScan);
	}

	heap_endscan(classScan);

	/* Release pg_class, hold pg_attribute for further processing */
	relation_close(classRel, ExclusiveLock);
	relation_close(attRel, NoLock);

	return rels;
}

/*
 * domainPermissionCheck
 *
 * Throw an error if the current user doesn't have permission to modify
 * the domain in an ALTER DOMAIN statement, or if the type isn't actually
 * a domain.
 */
void
domainPermissionCheck(HeapTuple tup, TypeName *typename)
{
	Form_pg_type	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Permission check: must own type or its namespace */
	if (!pg_type_ownercheck(HeapTupleGetOid(tup), GetUserId()) &&
		!pg_namespace_ownercheck(typTup->typnamespace,
								 GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, TypeNameToString(typename));

	/* Check that this is actually a domain */
	if (typTup->typtype != 'd')
		elog(ERROR, "%s is not a domain",
			 TypeNameToString(typename));
}


/*
 * domainAddConstraint - code shared between CREATE and ALTER DOMAIN
 */
static char *
domainAddConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
					int typMod, Constraint *constr,
					int *counter, char *domainName)
{
	Node	   *expr;
	char	   *ccsrc;
	char	   *ccbin;
	ParseState *pstate;
	ConstraintTestValue  *domVal;

	/*
	 * Assign or validate constraint name
	 */
	if (constr->name)
	{
		if (ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
								 domainOid,
								 domainNamespace,
								 constr->name))
			elog(ERROR, "constraint \"%s\" already exists for domain \"%s\"",
				 constr->name,
				 domainName);
	}
	else
		constr->name = GenerateConstraintName(CONSTRAINT_DOMAIN,
											  domainOid,
											  domainNamespace,
											  counter);

	/*
	 * Convert the A_EXPR in raw_expr into an EXPR
	 */
	pstate = make_parsestate(NULL);

	/*
	 * Set up a ConstraintTestValue to represent the occurrence of VALUE
	 * in the expression.  Note that it will appear to have the type of the
	 * base type, not the domain.  This seems correct since within the
	 * check expression, we should not assume the input value can be considered
	 * a member of the domain.
	 */
	domVal = makeNode(ConstraintTestValue);
	domVal->typeId = baseTypeOid;
	domVal->typeMod = typMod;

	pstate->p_value_substitute = (Node *) domVal;

	expr = transformExpr(pstate, constr->raw_expr);

	/*
	 * Make sure it yields a boolean result.
	 */
	expr = coerce_to_boolean(expr, "CHECK");

	/*
	 * Make sure no outside relations are
	 * referred to.
	 */
	if (length(pstate->p_rtable) != 0)
		elog(ERROR, "Relations cannot be referenced in domain CHECK constraint");

	/*
	 * Domains don't allow var clauses (this should be redundant with the
	 * above check, but make it anyway)
	 */
	if (contain_var_clause(expr))
		elog(ERROR, "cannot use column references in domain CHECK clause");

	/*
	 * No subplans or aggregates, either...
	 */
	if (contain_subplans(expr))
		elog(ERROR, "cannot use subselect in CHECK constraint expression");
	if (contain_agg_clause(expr))
		elog(ERROR, "cannot use aggregate function in CHECK constraint expression");

	/*
	 * Might as well try to reduce any constant expressions.
	 */
	expr = eval_const_expressions(expr);

	/*
	 * Convert to string form for storage.
	 */
	ccbin = nodeToString(expr);

	/*
	 * Deparse it to produce text for consrc.
	 *
	 * Since VARNOs aren't allowed in domain constraints, relation context
	 * isn't required as anything other than a shell.
	 */
	ccsrc = deparse_expression(expr,
							   deparse_context_for(domainName,
												   InvalidOid),
							   false, false);

	/*
	 * Store the constraint in pg_constraint
	 */
	CreateConstraintEntry(constr->name,		/* Constraint Name */
						  domainNamespace,	/* namespace */
						  CONSTRAINT_CHECK,		/* Constraint Type */
						  false,	/* Is Deferrable */
						  false,	/* Is Deferred */
						  InvalidOid,		/* not a relation constraint */
						  NULL,	
						  0,
						  domainOid,	/* domain constraint */
						  InvalidOid,	/* Foreign key fields */
						  NULL,
						  0,
						  ' ',
						  ' ',
						  ' ',
						  InvalidOid,
						  expr, 	/* Tree form check constraint */
						  ccbin,	/* Binary form check constraint */
						  ccsrc);	/* Source form check constraint */

	/*
	 * Return the compiled constraint expression so the calling routine can
	 * perform any additional required tests.
	 */
	return ccbin;
}
