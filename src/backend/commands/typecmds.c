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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/typecmds.c,v 1.10 2002/08/22 00:01:42 tgl Exp $
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
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Oid findTypeIOFunction(List *procname, Oid typeOid, bool isOutput);

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
			; /* ignored -- remove after 7.3 */
		else if (strcasecmp(defel->defname, "input") == 0)
			inputName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "output") == 0)
			outputName = defGetQualifiedName(defel);
		else if (strcasecmp(defel->defname, "send") == 0)
			; /* ignored -- remove after 7.3 */
		else if (strcasecmp(defel->defname, "receive") == 0)
			; /* ignored -- remove after 7.3 */
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
	 * TypeCreate will complain).  If it does then the declarations of the
	 * I/O functions might use it.
	 */
	typoid = GetSysCacheOid(TYPENAMENSP,
							CStringGetDatum(typeName),
							ObjectIdGetDatum(typeNamespace),
							0, 0);

	/*
	 * Convert I/O proc names to OIDs
	 */
	inputOid = findTypeIOFunction(inputName, typoid, false);
	outputOid = findTypeIOFunction(outputName, typoid, true);

	/*
	 * Verify that I/O procs return the expected thing.  OPAQUE is an allowed
	 * (but deprecated) alternative to the fully type-safe choices.
	 */
	resulttype = get_func_rettype(inputOid);
	if (!((OidIsValid(typoid) && resulttype == typoid) ||
		  resulttype == OPAQUEOID))
		elog(ERROR, "Type input function %s must return %s or OPAQUE",
			 NameListToString(inputName), typeName);
	resulttype = get_func_rettype(outputOid);
	if (!(resulttype == CSTRINGOID ||
		  resulttype == OPAQUEOID))
		elog(ERROR, "Type output function %s must return CSTRING or OPAQUE",
			 NameListToString(outputName));

	/*
	 * now have TypeCreate do all the real work.
	 */
	typoid =
		TypeCreate(typeName,		/* type name */
				   typeNamespace,	/* namespace */
				   InvalidOid,		/* preassigned type oid (not done here) */
				   InvalidOid,		/* relation oid (n/a here) */
				   internalLength,	/* internal size */
				   'b',				/* type-type (base type) */
				   delimiter,		/* array element delimiter */
				   inputOid,		/* input procedure */
				   outputOid,		/* output procedure */
				   elemType,		/* element type ID */
				   InvalidOid,		/* base type ID (only for domains) */
				   defaultValue,	/* default type value */
				   NULL,			/* no binary form available */
				   byValue,			/* passed by value */
				   alignment,		/* required alignment */
				   storage,			/* TOAST strategy */
				   -1,				/* typMod (Domains only) */
				   0,				/* Array Dimensions of typbasetype */
				   false);			/* Type NOT NULL */

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
	 * prefix.  So they can be one character longer.
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
	 * Base type must be a plain base type.  Domains over pseudo types would
	 * create a security hole.  Domains of domains might be made to work in
	 * the future, but not today.  Ditto for domains over complex types.
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
	datum =	SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefault, &isnull);
	if (!isnull)
		defaultValue = DatumGetCString(DirectFunctionCall1(textout, datum));

	/* Inherited default binary value */
	datum =	SysCacheGetAttr(TYPEOID, typeTup,
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
	 *
	 * Besides, we don't want any constraints to be cooked.  We'll
	 * do that when the table is created via MergeDomainAttributes().
	 */
	foreach(listptr, schema)
	{
		Constraint *colDef = lfirst(listptr);
		ParseState *pstate;

		switch (colDef->contype)
		{
			/*
	 		 * The inherited default value may be overridden by the user
			 * with the DEFAULT <expr> statement.
			 *
	 		 * We have to search the entire constraint tree returned as we
			 * don't want to cook or fiddle too much.
			 */
			case CONSTR_DEFAULT:
				if (defaultExpr)
					elog(ERROR, "CREATE DOMAIN has multiple DEFAULT expressions");
				/* Create a dummy ParseState for transformExpr */
				pstate = make_parsestate(NULL);
				/*
				 * Cook the colDef->raw_expr into an expression.
				 * Note: Name is strictly for error message
				 */
				defaultExpr = cookDefault(pstate, colDef->raw_expr,
										  basetypeoid,
										  stmt->typename->typmod,
										  domainName);
				/*
				 * Expression must be stored as a nodeToString result,
				 * but we also require a valid textual representation
				 * (mainly to make life easier for pg_dump).
				 */
				defaultValue = deparse_expression(defaultExpr,
								deparse_context_for(domainName,
													InvalidOid),
												   false);
				defaultValueBin = nodeToString(defaultExpr);
				break;

			/*
			 * Find the NULL constraint.
			 */
			case CONSTR_NOTNULL:
				if (nullDefined)
					elog(ERROR, "CREATE DOMAIN has conflicting NULL / NOT NULL constraint");
				typNotNull = true;
				nullDefined = true;
		  		break;

			case CONSTR_NULL:
				if (nullDefined)
					elog(ERROR, "CREATE DOMAIN has conflicting NULL / NOT NULL constraint");
				typNotNull = false;
				nullDefined = true;
		  		break;

		  	case CONSTR_UNIQUE:
		  		elog(ERROR, "CREATE DOMAIN / UNIQUE indexes not supported");
		  		break;

		  	case CONSTR_PRIMARY:
		  		elog(ERROR, "CREATE DOMAIN / PRIMARY KEY indexes not supported");
		  		break;

		  	case CONSTR_CHECK:
		  		elog(ERROR, "DefineDomain: CHECK Constraints not supported");
		  		break;

		  	case CONSTR_ATTR_DEFERRABLE:
		  	case CONSTR_ATTR_NOT_DEFERRABLE:
		  	case CONSTR_ATTR_DEFERRED:
		  	case CONSTR_ATTR_IMMEDIATE:
		  		elog(ERROR, "DefineDomain: DEFERRABLE, NON DEFERRABLE, DEFERRED and IMMEDIATE not supported");
		  		break;

			default:
		  		elog(ERROR, "DefineDomain: unrecognized constraint node type");
		  		break;
		}
	}

	/*
	 * Have TypeCreate do all the real work.
	 */
	domainoid =
		TypeCreate(domainName,			/* type name */
				   domainNamespace,		/* namespace */
				   InvalidOid,			/* preassigned type oid (none here) */
				   InvalidOid,			/* relation oid (n/a here) */
				   internalLength,		/* internal size */
				   'd',					/* type-type (domain type) */
				   delimiter,			/* array element delimiter */
				   inputProcedure,		/* input procedure */
				   outputProcedure,		/* output procedure */
				   basetypelem,			/* element type ID */
				   basetypeoid,			/* base type ID */
				   defaultValue,		/* default type value (text) */
				   defaultValueBin,		/* default type value (binary) */
				   byValue,				/* passed by value */
				   alignment,			/* required alignment */
				   storage,				/* TOAST strategy */
				   stmt->typename->typmod, /* typeMod value */
				   typNDims,			/* Array dimensions for base type */
				   typNotNull);			/* Type NOT NULL */

	/*
	 * Add any dependencies needed for the default expression.
	 */
	if (defaultExpr)
	{
		ObjectAddress	domobject;

		domobject.classId = RelOid_pg_type;
		domobject.objectId = domainoid;
		domobject.objectSubId = 0;

		recordDependencyOnExpr(&domobject, defaultExpr, NIL,
							   DEPENDENCY_NORMAL);
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
 * typeOid is the type's OID, if it already exists as a shell type,
 * otherwise InvalidOid.
 */
static Oid
findTypeIOFunction(List *procname, Oid typeOid, bool isOutput)
{
	Oid			argList[FUNC_MAX_ARGS];
	Oid			procOid;

	if (isOutput)
	{
		/*
		 * Output functions can take a single argument of the type,
		 * or two arguments (data value, element OID).  The signature
		 * may use OPAQUE in place of the actual type name; this is the
		 * only possibility if the type doesn't yet exist as a shell.
		 */
		if (OidIsValid(typeOid))
		{
			MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

			argList[0] = typeOid;

			procOid = LookupFuncName(procname, 1, argList);
			if (OidIsValid(procOid))
				return procOid;

			argList[1] = OIDOID;

			procOid = LookupFuncName(procname, 2, argList);
			if (OidIsValid(procOid))
				return procOid;

		}

		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		argList[0] = OPAQUEOID;

		procOid = LookupFuncName(procname, 1, argList);
		if (OidIsValid(procOid))
			return procOid;

		argList[1] = OIDOID;

		procOid = LookupFuncName(procname, 2, argList);
		if (OidIsValid(procOid))
			return procOid;

		/* Prefer type name over OPAQUE in the failure message. */
		if (OidIsValid(typeOid))
			argList[0] = typeOid;

		func_error("TypeCreate", procname, 1, argList, NULL);
	}
	else
	{
		/*
		 * Input functions can take a single argument of type CSTRING,
		 * or three arguments (string, element OID, typmod).  The signature
		 * may use OPAQUE in place of CSTRING.
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

		MemSet(argList, 0, FUNC_MAX_ARGS * sizeof(Oid));

		argList[0] = OPAQUEOID;

		procOid = LookupFuncName(procname, 1, argList);
		if (OidIsValid(procOid))
			return procOid;

		argList[1] = OIDOID;
		argList[2] = INT4OID;

		procOid = LookupFuncName(procname, 3, argList);
		if (OidIsValid(procOid))
			return procOid;

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

	/*
	 * finally create the relation...
	 */
	return DefineRelation(createStmt, RELKIND_COMPOSITE_TYPE);
}
