/*-------------------------------------------------------------------------
 *
 * typecmds.c
 *	  Routines for SQL commands that manipulate types (and domains).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/typecmds.c
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
 *				input/output, recv/send functions
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* result structure for get_rels_with_domain() */
typedef struct
{
	Relation	rel;			/* opened and locked relation */
	int			natts;			/* number of attributes of interest */
	int		   *atts;			/* attribute numbers */
	/* atts[] is of allocated length RelationGetNumberOfAttributes(rel) */
} RelToCheck;

/* parameter structure for AlterTypeRecurse() */
typedef struct
{
	/* Flags indicating which type attributes to update */
	bool		updateStorage;
	bool		updateReceive;
	bool		updateSend;
	bool		updateTypmodin;
	bool		updateTypmodout;
	bool		updateAnalyze;
	bool		updateSubscript;
	/* New values for relevant attributes */
	char		storage;
	Oid			receiveOid;
	Oid			sendOid;
	Oid			typmodinOid;
	Oid			typmodoutOid;
	Oid			analyzeOid;
	Oid			subscriptOid;
} AlterTypeRecurseParams;

/* Potentially set by pg_upgrade_support functions */
Oid			binary_upgrade_next_array_pg_type_oid = InvalidOid;
Oid			binary_upgrade_next_mrng_pg_type_oid = InvalidOid;
Oid			binary_upgrade_next_mrng_array_pg_type_oid = InvalidOid;

static void makeRangeConstructors(const char *name, Oid namespace,
								  Oid rangeOid, Oid subtype);
static void makeMultirangeConstructors(const char *name, Oid namespace,
									   Oid multirangeOid, Oid rangeOid,
									   Oid rangeArrayOid, Oid *castFuncOid);
static Oid	findTypeInputFunction(List *procname, Oid typeOid);
static Oid	findTypeOutputFunction(List *procname, Oid typeOid);
static Oid	findTypeReceiveFunction(List *procname, Oid typeOid);
static Oid	findTypeSendFunction(List *procname, Oid typeOid);
static Oid	findTypeTypmodinFunction(List *procname);
static Oid	findTypeTypmodoutFunction(List *procname);
static Oid	findTypeAnalyzeFunction(List *procname, Oid typeOid);
static Oid	findTypeSubscriptingFunction(List *procname, Oid typeOid);
static Oid	findRangeSubOpclass(List *opcname, Oid subtype);
static Oid	findRangeCanonicalFunction(List *procname, Oid typeOid);
static Oid	findRangeSubtypeDiffFunction(List *procname, Oid subtype);
static void validateDomainCheckConstraint(Oid domainoid, const char *ccbin);
static void validateDomainNotNullConstraint(Oid domainoid);
static List *get_rels_with_domain(Oid domainOid, LOCKMODE lockmode);
static void checkEnumOwner(HeapTuple tup);
static char *domainAddCheckConstraint(Oid domainOid, Oid domainNamespace,
									  Oid baseTypeOid,
									  int typMod, Constraint *constr,
									  const char *domainName, ObjectAddress *constrAddr);
static Node *replace_domain_constraint_value(ParseState *pstate,
											 ColumnRef *cref);
static void domainAddNotNullConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
									   int typMod, Constraint *constr,
									   const char *domainName, ObjectAddress *constrAddr);
static void AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
							 HeapTuple tup, Relation catalog,
							 AlterTypeRecurseParams *atparams);


/*
 * DefineType
 *		Registers a new base type.
 */
ObjectAddress
DefineType(ParseState *pstate, List *names, List *parameters)
{
	char	   *typeName;
	Oid			typeNamespace;
	int16		internalLength = -1;	/* default: variable-length */
	List	   *inputName = NIL;
	List	   *outputName = NIL;
	List	   *receiveName = NIL;
	List	   *sendName = NIL;
	List	   *typmodinName = NIL;
	List	   *typmodoutName = NIL;
	List	   *analyzeName = NIL;
	List	   *subscriptName = NIL;
	char		category = TYPCATEGORY_USER;
	bool		preferred = false;
	char		delimiter = DEFAULT_TYPDELIM;
	Oid			elemType = InvalidOid;
	char	   *defaultValue = NULL;
	bool		byValue = false;
	char		alignment = TYPALIGN_INT;	/* default alignment */
	char		storage = TYPSTORAGE_PLAIN; /* default TOAST storage method */
	Oid			collation = InvalidOid;
	DefElem    *likeTypeEl = NULL;
	DefElem    *internalLengthEl = NULL;
	DefElem    *inputNameEl = NULL;
	DefElem    *outputNameEl = NULL;
	DefElem    *receiveNameEl = NULL;
	DefElem    *sendNameEl = NULL;
	DefElem    *typmodinNameEl = NULL;
	DefElem    *typmodoutNameEl = NULL;
	DefElem    *analyzeNameEl = NULL;
	DefElem    *subscriptNameEl = NULL;
	DefElem    *categoryEl = NULL;
	DefElem    *preferredEl = NULL;
	DefElem    *delimiterEl = NULL;
	DefElem    *elemTypeEl = NULL;
	DefElem    *defaultValueEl = NULL;
	DefElem    *byValueEl = NULL;
	DefElem    *alignmentEl = NULL;
	DefElem    *storageEl = NULL;
	DefElem    *collatableEl = NULL;
	Oid			inputOid;
	Oid			outputOid;
	Oid			receiveOid = InvalidOid;
	Oid			sendOid = InvalidOid;
	Oid			typmodinOid = InvalidOid;
	Oid			typmodoutOid = InvalidOid;
	Oid			analyzeOid = InvalidOid;
	Oid			subscriptOid = InvalidOid;
	char	   *array_type;
	Oid			array_oid;
	Oid			typoid;
	ListCell   *pl;
	ObjectAddress address;

	/*
	 * As of Postgres 8.4, we require superuser privilege to create a base
	 * type.  This is simple paranoia: there are too many ways to mess up the
	 * system with an incorrect type definition (for instance, representation
	 * parameters that don't match what the C code expects).  In practice it
	 * takes superuser privilege to create the I/O functions, and so the
	 * former requirement that you own the I/O functions pretty much forced
	 * superuserness anyway.  We're just making doubly sure here.
	 *
	 * XXX re-enable NOT_USED code sections below if you remove this test.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create a base type")));

	/* Convert list of names to a name and namespace */
	typeNamespace = QualifiedNameGetCreationNamespace(names, &typeName);

#ifdef NOT_USED
	/* XXX this is unnecessary given the superuser check above */
	/* Check we have creation rights in target namespace */
	aclresult = object_aclcheck(NamespaceRelationId, typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));
#endif

	/*
	 * Look to see if type already exists.
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/*
	 * If this command is a parameterless CREATE TYPE, then we're just here to
	 * make a shell type, so do that (or fail if there already is a shell).
	 */
	if (parameters == NIL)
	{
		if (OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		address = TypeShellMake(typeName, typeNamespace, GetUserId());
		return address;
	}

	/*
	 * Otherwise, we must already have a shell type, since there is no other
	 * way that the I/O functions could have been created.
	 */
	if (!OidIsValid(typoid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" does not exist", typeName),
				 errhint("Create the type as a shell type, then create its I/O functions, then do a full CREATE TYPE.")));

	/* Extract the parameters from the parameter list */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "like") == 0)
			defelp = &likeTypeEl;
		else if (strcmp(defel->defname, "internallength") == 0)
			defelp = &internalLengthEl;
		else if (strcmp(defel->defname, "input") == 0)
			defelp = &inputNameEl;
		else if (strcmp(defel->defname, "output") == 0)
			defelp = &outputNameEl;
		else if (strcmp(defel->defname, "receive") == 0)
			defelp = &receiveNameEl;
		else if (strcmp(defel->defname, "send") == 0)
			defelp = &sendNameEl;
		else if (strcmp(defel->defname, "typmod_in") == 0)
			defelp = &typmodinNameEl;
		else if (strcmp(defel->defname, "typmod_out") == 0)
			defelp = &typmodoutNameEl;
		else if (strcmp(defel->defname, "analyze") == 0 ||
				 strcmp(defel->defname, "analyse") == 0)
			defelp = &analyzeNameEl;
		else if (strcmp(defel->defname, "subscript") == 0)
			defelp = &subscriptNameEl;
		else if (strcmp(defel->defname, "category") == 0)
			defelp = &categoryEl;
		else if (strcmp(defel->defname, "preferred") == 0)
			defelp = &preferredEl;
		else if (strcmp(defel->defname, "delimiter") == 0)
			defelp = &delimiterEl;
		else if (strcmp(defel->defname, "element") == 0)
			defelp = &elemTypeEl;
		else if (strcmp(defel->defname, "default") == 0)
			defelp = &defaultValueEl;
		else if (strcmp(defel->defname, "passedbyvalue") == 0)
			defelp = &byValueEl;
		else if (strcmp(defel->defname, "alignment") == 0)
			defelp = &alignmentEl;
		else if (strcmp(defel->defname, "storage") == 0)
			defelp = &storageEl;
		else if (strcmp(defel->defname, "collatable") == 0)
			defelp = &collatableEl;
		else
		{
			/* WARNING, not ERROR, for historical backwards-compatibility */
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			continue;
		}
		if (*defelp != NULL)
			errorConflictingDefElem(defel, pstate);
		*defelp = defel;
	}

	/*
	 * Now interpret the options; we do this separately so that LIKE can be
	 * overridden by other options regardless of the ordering in the parameter
	 * list.
	 */
	if (likeTypeEl)
	{
		Type		likeType;
		Form_pg_type likeForm;

		likeType = typenameType(NULL, defGetTypeName(likeTypeEl), NULL);
		likeForm = (Form_pg_type) GETSTRUCT(likeType);
		internalLength = likeForm->typlen;
		byValue = likeForm->typbyval;
		alignment = likeForm->typalign;
		storage = likeForm->typstorage;
		ReleaseSysCache(likeType);
	}
	if (internalLengthEl)
		internalLength = defGetTypeLength(internalLengthEl);
	if (inputNameEl)
		inputName = defGetQualifiedName(inputNameEl);
	if (outputNameEl)
		outputName = defGetQualifiedName(outputNameEl);
	if (receiveNameEl)
		receiveName = defGetQualifiedName(receiveNameEl);
	if (sendNameEl)
		sendName = defGetQualifiedName(sendNameEl);
	if (typmodinNameEl)
		typmodinName = defGetQualifiedName(typmodinNameEl);
	if (typmodoutNameEl)
		typmodoutName = defGetQualifiedName(typmodoutNameEl);
	if (analyzeNameEl)
		analyzeName = defGetQualifiedName(analyzeNameEl);
	if (subscriptNameEl)
		subscriptName = defGetQualifiedName(subscriptNameEl);
	if (categoryEl)
	{
		char	   *p = defGetString(categoryEl);

		category = p[0];
		/* restrict to non-control ASCII */
		if (category < 32 || category > 126)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid type category \"%s\": must be simple ASCII",
							p)));
	}
	if (preferredEl)
		preferred = defGetBoolean(preferredEl);
	if (delimiterEl)
	{
		char	   *p = defGetString(delimiterEl);

		delimiter = p[0];
		/* XXX shouldn't we restrict the delimiter? */
	}
	if (elemTypeEl)
	{
		elemType = typenameTypeId(NULL, defGetTypeName(elemTypeEl));
		/* disallow arrays of pseudotypes */
		if (get_typtype(elemType) == TYPTYPE_PSEUDO)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("array element type cannot be %s",
							format_type_be(elemType))));
	}
	if (defaultValueEl)
		defaultValue = defGetString(defaultValueEl);
	if (byValueEl)
		byValue = defGetBoolean(byValueEl);
	if (alignmentEl)
	{
		char	   *a = defGetString(alignmentEl);

		/*
		 * Note: if argument was an unquoted identifier, parser will have
		 * applied translations to it, so be prepared to recognize translated
		 * type names as well as the nominal form.
		 */
		if (pg_strcasecmp(a, "double") == 0 ||
			pg_strcasecmp(a, "float8") == 0 ||
			pg_strcasecmp(a, "pg_catalog.float8") == 0)
			alignment = TYPALIGN_DOUBLE;
		else if (pg_strcasecmp(a, "int4") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.int4") == 0)
			alignment = TYPALIGN_INT;
		else if (pg_strcasecmp(a, "int2") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.int2") == 0)
			alignment = TYPALIGN_SHORT;
		else if (pg_strcasecmp(a, "char") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.bpchar") == 0)
			alignment = TYPALIGN_CHAR;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("alignment \"%s\" not recognized", a)));
	}
	if (storageEl)
	{
		char	   *a = defGetString(storageEl);

		if (pg_strcasecmp(a, "plain") == 0)
			storage = TYPSTORAGE_PLAIN;
		else if (pg_strcasecmp(a, "external") == 0)
			storage = TYPSTORAGE_EXTERNAL;
		else if (pg_strcasecmp(a, "extended") == 0)
			storage = TYPSTORAGE_EXTENDED;
		else if (pg_strcasecmp(a, "main") == 0)
			storage = TYPSTORAGE_MAIN;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("storage \"%s\" not recognized", a)));
	}
	if (collatableEl)
		collation = defGetBoolean(collatableEl) ? DEFAULT_COLLATION_OID : InvalidOid;

	/*
	 * make sure we have our required definitions
	 */
	if (inputName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function must be specified")));
	if (outputName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function must be specified")));

	if (typmodinName == NIL && typmodoutName != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function is useless without a type modifier input function")));

	/*
	 * Convert I/O proc names to OIDs
	 */
	inputOid = findTypeInputFunction(inputName, typoid);
	outputOid = findTypeOutputFunction(outputName, typoid);
	if (receiveName)
		receiveOid = findTypeReceiveFunction(receiveName, typoid);
	if (sendName)
		sendOid = findTypeSendFunction(sendName, typoid);

	/*
	 * Convert typmodin/out function proc names to OIDs.
	 */
	if (typmodinName)
		typmodinOid = findTypeTypmodinFunction(typmodinName);
	if (typmodoutName)
		typmodoutOid = findTypeTypmodoutFunction(typmodoutName);

	/*
	 * Convert analysis function proc name to an OID. If no analysis function
	 * is specified, we'll use zero to select the built-in default algorithm.
	 */
	if (analyzeName)
		analyzeOid = findTypeAnalyzeFunction(analyzeName, typoid);

	/*
	 * Likewise look up the subscripting function if any.  If it is not
	 * specified, but a typelem is specified, allow that if
	 * raw_array_subscript_handler can be used.  (This is for backwards
	 * compatibility; maybe someday we should throw an error instead.)
	 */
	if (subscriptName)
		subscriptOid = findTypeSubscriptingFunction(subscriptName, typoid);
	else if (OidIsValid(elemType))
	{
		if (internalLength > 0 && !byValue && get_typlen(elemType) > 0)
			subscriptOid = F_RAW_ARRAY_SUBSCRIPT_HANDLER;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("element type cannot be specified without a subscripting function")));
	}

	/*
	 * Check permissions on functions.  We choose to require the creator/owner
	 * of a type to also own the underlying functions.  Since creating a type
	 * is tantamount to granting public execute access on the functions, the
	 * minimum sane check would be for execute-with-grant-option.  But we
	 * don't have a way to make the type go away if the grant option is
	 * revoked, so ownership seems better.
	 *
	 * XXX For now, this is all unnecessary given the superuser check above.
	 * If we ever relax that, these calls likely should be moved into
	 * findTypeInputFunction et al, where they could be shared by AlterType.
	 */
#ifdef NOT_USED
	if (inputOid && !object_ownercheck(ProcedureRelationId, inputOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(inputName));
	if (outputOid && !object_ownercheck(ProcedureRelationId, outputOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(outputName));
	if (receiveOid && !object_ownercheck(ProcedureRelationId, receiveOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(receiveName));
	if (sendOid && !object_ownercheck(ProcedureRelationId, sendOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(sendName));
	if (typmodinOid && !object_ownercheck(ProcedureRelationId, typmodinOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(typmodinName));
	if (typmodoutOid && !object_ownercheck(ProcedureRelationId, typmodoutOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(typmodoutName));
	if (analyzeOid && !object_ownercheck(ProcedureRelationId, analyzeOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(analyzeName));
	if (subscriptOid && !object_ownercheck(ProcedureRelationId, subscriptOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(subscriptName));
#endif

	/*
	 * OK, we're done checking, time to make the type.  We must assign the
	 * array type OID ahead of calling TypeCreate, since the base type and
	 * array type each refer to the other.
	 */
	array_oid = AssignTypeArrayOid();

	/*
	 * now have TypeCreate do all the real work.
	 *
	 * Note: the pg_type.oid is stored in user tables as array elements (base
	 * types) in ArrayType and in composite types in DatumTupleFields.  This
	 * oid must be preserved by binary upgrades.
	 */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   typeName,	/* type name */
				   typeNamespace,	/* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   internalLength,	/* internal size */
				   TYPTYPE_BASE,	/* type-type (base type) */
				   category,	/* type-category */
				   preferred,	/* is it a preferred type? */
				   delimiter,	/* array element delimiter */
				   inputOid,	/* input procedure */
				   outputOid,	/* output procedure */
				   receiveOid,	/* receive procedure */
				   sendOid,		/* send procedure */
				   typmodinOid, /* typmodin procedure */
				   typmodoutOid,	/* typmodout procedure */
				   analyzeOid,	/* analyze procedure */
				   subscriptOid,	/* subscript procedure */
				   elemType,	/* element type ID */
				   false,		/* this is not an implicit array type */
				   array_oid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   defaultValue,	/* default type value */
				   NULL,		/* no binary form available */
				   byValue,		/* passed by value */
				   alignment,	/* required alignment */
				   storage,		/* TOAST strategy */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array Dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   collation);	/* type's collation */
	Assert(typoid == address.objectId);

	/*
	 * Create the array type that goes with it.
	 */
	array_type = makeArrayTypeName(typeName, typeNamespace);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for arrays */
	alignment = (alignment == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	TypeCreate(array_oid,		/* force assignment of this type OID */
			   array_type,		/* type name */
			   typeNamespace,	/* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   delimiter,		/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   typmodinOid,		/* typmodin procedure */
			   typmodoutOid,	/* typmodout procedure */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   typoid,			/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* see above */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   collation);		/* type's collation */

	pfree(array_type);

	return address;
}

/*
 * Guts of type deletion.
 */
void
RemoveTypeById(Oid typeOid)
{
	Relation	relation;
	HeapTuple	tup;

	relation = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	CatalogTupleDelete(relation, &tup->t_self);

	/*
	 * If it is an enum, delete the pg_enum entries too; we don't bother with
	 * making dependency entries for those, so it has to be done "by hand"
	 * here.
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_ENUM)
		EnumValuesDelete(typeOid);

	/*
	 * If it is a range type, delete the pg_range entry too; we don't bother
	 * with making a dependency entry for that, so it has to be done "by hand"
	 * here.
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_RANGE)
		RangeDelete(typeOid);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}


/*
 * DefineDomain
 *		Registers a new domain.
 */
ObjectAddress
DefineDomain(CreateDomainStmt *stmt)
{
	char	   *domainName;
	char	   *domainArrayName;
	Oid			domainNamespace;
	AclResult	aclresult;
	int16		internalLength;
	Oid			inputProcedure;
	Oid			outputProcedure;
	Oid			receiveProcedure;
	Oid			sendProcedure;
	Oid			analyzeProcedure;
	bool		byValue;
	char		category;
	char		delimiter;
	char		alignment;
	char		storage;
	char		typtype;
	Datum		datum;
	bool		isnull;
	char	   *defaultValue = NULL;
	char	   *defaultValueBin = NULL;
	bool		saw_default = false;
	bool		typNotNull = false;
	bool		nullDefined = false;
	int32		typNDims = list_length(stmt->typeName->arrayBounds);
	HeapTuple	typeTup;
	List	   *schema = stmt->constraints;
	ListCell   *listptr;
	Oid			basetypeoid;
	Oid			old_type_oid;
	Oid			domaincoll;
	Oid			domainArrayOid;
	Form_pg_type baseType;
	int32		basetypeMod;
	Oid			baseColl;
	ObjectAddress address;

	/* Convert list of names to a name and namespace */
	domainNamespace = QualifiedNameGetCreationNamespace(stmt->domainname,
														&domainName);

	/* Check we have creation rights in target namespace */
	aclresult = object_aclcheck(NamespaceRelationId, domainNamespace, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(domainNamespace));

	/*
	 * Check for collision with an existing type name.  If there is one and
	 * it's an autogenerated array, we can rename it out of the way.
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(domainName),
								   ObjectIdGetDatum(domainNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, domainName, domainNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", domainName)));
	}

	/*
	 * Look up the base type.
	 */
	typeTup = typenameType(NULL, stmt->typeName, &basetypeMod);
	baseType = (Form_pg_type) GETSTRUCT(typeTup);
	basetypeoid = baseType->oid;

	/*
	 * Base type must be a plain base type, a composite type, another domain,
	 * an enum or a range type.  Domains over pseudotypes would create a
	 * security hole.  (It would be shorter to code this to just check for
	 * pseudotypes; but it seems safer to call out the specific typtypes that
	 * are supported, rather than assume that all future typtypes would be
	 * automatically supported.)
	 */
	typtype = baseType->typtype;
	if (typtype != TYPTYPE_BASE &&
		typtype != TYPTYPE_COMPOSITE &&
		typtype != TYPTYPE_DOMAIN &&
		typtype != TYPTYPE_ENUM &&
		typtype != TYPTYPE_RANGE &&
		typtype != TYPTYPE_MULTIRANGE)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("\"%s\" is not a valid base type for a domain",
						TypeNameToString(stmt->typeName))));

	aclresult = object_aclcheck(TypeRelationId, basetypeoid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, basetypeoid);

	/*
	 * Collect the properties of the new domain.  Some are inherited from the
	 * base type, some are not.  If you change any of this inheritance
	 * behavior, be sure to update AlterTypeRecurse() to match!
	 */

	/*
	 * Identify the collation if any
	 */
	baseColl = baseType->typcollation;
	if (stmt->collClause)
		domaincoll = get_collation_oid(stmt->collClause->collname, false);
	else
		domaincoll = baseColl;

	/* Complain if COLLATE is applied to an uncollatable type */
	if (OidIsValid(domaincoll) && !OidIsValid(baseColl))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(basetypeoid))));

	/* passed by value */
	byValue = baseType->typbyval;

	/* Required Alignment */
	alignment = baseType->typalign;

	/* TOAST Strategy */
	storage = baseType->typstorage;

	/* Storage Length */
	internalLength = baseType->typlen;

	/* Type Category */
	category = baseType->typcategory;

	/* Array element Delimiter */
	delimiter = baseType->typdelim;

	/* I/O Functions */
	inputProcedure = F_DOMAIN_IN;
	outputProcedure = baseType->typoutput;
	receiveProcedure = F_DOMAIN_RECV;
	sendProcedure = baseType->typsend;

	/* Domains never accept typmods, so no typmodin/typmodout needed */

	/* Analysis function */
	analyzeProcedure = baseType->typanalyze;

	/*
	 * Domains don't need a subscript function, since they are not
	 * subscriptable on their own.  If the base type is subscriptable, the
	 * parser will reduce the type to the base type before subscripting.
	 */

	/* Inherited default value */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefault, &isnull);
	if (!isnull)
		defaultValue = TextDatumGetCString(datum);

	/* Inherited default binary value */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefaultbin, &isnull);
	if (!isnull)
		defaultValueBin = TextDatumGetCString(datum);

	/*
	 * Run through constraints manually to avoid the additional processing
	 * conducted by DefineRelation() and friends.
	 */
	foreach(listptr, schema)
	{
		Constraint *constr = lfirst(listptr);

		if (!IsA(constr, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(constr));
		switch (constr->contype)
		{
			case CONSTR_DEFAULT:

				/*
				 * The inherited default value may be overridden by the user
				 * with the DEFAULT <expr> clause ... but only once.
				 */
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default expressions")));
				saw_default = true;

				if (constr->raw_expr)
				{
					ParseState *pstate;
					Node	   *defaultExpr;

					/* Create a dummy ParseState for transformExpr */
					pstate = make_parsestate(NULL);

					/*
					 * Cook the constr->raw_expr into an expression. Note:
					 * name is strictly for error message
					 */
					defaultExpr = cookDefault(pstate, constr->raw_expr,
											  basetypeoid,
											  basetypeMod,
											  domainName,
											  0);

					/*
					 * If the expression is just a NULL constant, we treat it
					 * like not having a default.
					 *
					 * Note that if the basetype is another domain, we'll see
					 * a CoerceToDomain expr here and not discard the default.
					 * This is critical because the domain default needs to be
					 * retained to override any default that the base domain
					 * might have.
					 */
					if (defaultExpr == NULL ||
						(IsA(defaultExpr, Const) &&
						 ((Const *) defaultExpr)->constisnull))
					{
						defaultValue = NULL;
						defaultValueBin = NULL;
					}
					else
					{
						/*
						 * Expression must be stored as a nodeToString result,
						 * but we also require a valid textual representation
						 * (mainly to make life easier for pg_dump).
						 */
						defaultValue =
							deparse_expression(defaultExpr,
											   NIL, false, false);
						defaultValueBin = nodeToString(defaultExpr);
					}
				}
				else
				{
					/* No default (can this still happen?) */
					defaultValue = NULL;
					defaultValueBin = NULL;
				}
				break;

			case CONSTR_NOTNULL:
				if (nullDefined && !typNotNull)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL constraints")));
				if (constr->is_no_inherit)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("not-null constraints for domains cannot be marked NO INHERIT"));
				typNotNull = true;
				nullDefined = true;
				break;

			case CONSTR_NULL:
				if (nullDefined && typNotNull)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL constraints")));
				typNotNull = false;
				nullDefined = true;
				break;

			case CONSTR_CHECK:

				/*
				 * Check constraints are handled after domain creation, as
				 * they require the Oid of the domain; at this point we can
				 * only check that they're not marked NO INHERIT, because that
				 * would be bogus.
				 */
				if (constr->is_no_inherit)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("check constraints for domains cannot be marked NO INHERIT")));
				break;

				/*
				 * All else are error cases
				 */
			case CONSTR_UNIQUE:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unique constraints not possible for domains")));
				break;

			case CONSTR_PRIMARY:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("primary key constraints not possible for domains")));
				break;

			case CONSTR_EXCLUSION:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("exclusion constraints not possible for domains")));
				break;

			case CONSTR_FOREIGN:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("foreign key constraints not possible for domains")));
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("specifying constraint deferrability not supported for domains")));
				break;

			default:
				elog(ERROR, "unrecognized constraint subtype: %d",
					 (int) constr->contype);
				break;
		}
	}

	/* Allocate OID for array type */
	domainArrayOid = AssignTypeArrayOid();

	/*
	 * Have TypeCreate do all the real work.
	 */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   domainName,	/* type name */
				   domainNamespace, /* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   internalLength,	/* internal size */
				   TYPTYPE_DOMAIN,	/* type-type (domain type) */
				   category,	/* type-category */
				   false,		/* domain types are never preferred */
				   delimiter,	/* array element delimiter */
				   inputProcedure,	/* input procedure */
				   outputProcedure, /* output procedure */
				   receiveProcedure,	/* receive procedure */
				   sendProcedure,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   analyzeProcedure,	/* analyze procedure */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* no array element type */
				   false,		/* this isn't an array */
				   domainArrayOid,	/* array type we are about to create */
				   basetypeoid, /* base type ID */
				   defaultValue,	/* default type value (text) */
				   defaultValueBin, /* default type value (binary) */
				   byValue,		/* passed by value */
				   alignment,	/* required alignment */
				   storage,		/* TOAST strategy */
				   basetypeMod, /* typeMod value */
				   typNDims,	/* Array dimensions for base type */
				   typNotNull,	/* Type NOT NULL */
				   domaincoll); /* type's collation */

	/*
	 * Create the array type that goes with it.
	 */
	domainArrayName = makeArrayTypeName(domainName, domainNamespace);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for arrays */
	alignment = (alignment == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	TypeCreate(domainArrayOid,	/* force assignment of this type OID */
			   domainArrayName, /* type name */
			   domainNamespace, /* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   delimiter,		/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure - none */
			   InvalidOid,		/* typmodout procedure - none */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   address.objectId,	/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* see above */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   domaincoll);		/* type's collation */

	pfree(domainArrayName);

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
				domainAddCheckConstraint(address.objectId, domainNamespace,
										 basetypeoid, basetypeMod,
										 constr, domainName, NULL);
				break;

			case CONSTR_NOTNULL:
				domainAddNotNullConstraint(address.objectId, domainNamespace,
										   basetypeoid, basetypeMod,
										   constr, domainName, NULL);
				break;

				/* Other constraint types were fully processed above */

			default:
				break;
		}

		/* CCI so we can detect duplicate constraint names */
		CommandCounterIncrement();
	}

	/*
	 * Now we can clean up.
	 */
	ReleaseSysCache(typeTup);

	return address;
}


/*
 * DefineEnum
 *		Registers a new enum.
 */
ObjectAddress
DefineEnum(CreateEnumStmt *stmt)
{
	char	   *enumName;
	char	   *enumArrayName;
	Oid			enumNamespace;
	AclResult	aclresult;
	Oid			old_type_oid;
	Oid			enumArrayOid;
	ObjectAddress enumTypeAddr;

	/* Convert list of names to a name and namespace */
	enumNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &enumName);

	/* Check we have creation rights in target namespace */
	aclresult = object_aclcheck(NamespaceRelationId, enumNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(enumNamespace));

	/*
	 * Check for collision with an existing type name.  If there is one and
	 * it's an autogenerated array, we can rename it out of the way.
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(enumName),
								   ObjectIdGetDatum(enumNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, enumName, enumNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", enumName)));
	}

	/* Allocate OID for array type */
	enumArrayOid = AssignTypeArrayOid();

	/* Create the pg_type entry */
	enumTypeAddr =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   enumName,	/* type name */
				   enumNamespace,	/* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   sizeof(Oid), /* internal size */
				   TYPTYPE_ENUM,	/* type-type (enum type) */
				   TYPCATEGORY_ENUM,	/* type-category (enum type) */
				   false,		/* enum types are never preferred */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   F_ENUM_IN,	/* input procedure */
				   F_ENUM_OUT,	/* output procedure */
				   F_ENUM_RECV, /* receive procedure */
				   F_ENUM_SEND, /* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   InvalidOid,	/* analyze procedure - default */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* element type ID */
				   false,		/* this is not an array type */
				   enumArrayOid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* never a default type value */
				   NULL,		/* binary default isn't sent either */
				   true,		/* always passed by value */
				   TYPALIGN_INT,	/* int alignment */
				   TYPSTORAGE_PLAIN,	/* TOAST strategy always plain */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation */

	/* Enter the enum's values into pg_enum */
	EnumValuesCreate(enumTypeAddr.objectId, stmt->vals);

	/*
	 * Create the array type that goes with it.
	 */
	enumArrayName = makeArrayTypeName(enumName, enumNamespace);

	TypeCreate(enumArrayOid,	/* force assignment of this type OID */
			   enumArrayName,	/* type name */
			   enumNamespace,	/* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure - none */
			   InvalidOid,		/* typmodout procedure - none */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   enumTypeAddr.objectId,	/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   TYPALIGN_INT,	/* enums have int align, so do their arrays */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* type's collation */

	pfree(enumArrayName);

	return enumTypeAddr;
}

/*
 * AlterEnum
 *		Adds a new label to an existing enum.
 */
ObjectAddress
AlterEnum(AlterEnumStmt *stmt)
{
	Oid			enum_type_oid;
	TypeName   *typename;
	HeapTuple	tup;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(stmt->typeName);
	enum_type_oid = typenameTypeId(NULL, typename);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(enum_type_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", enum_type_oid);

	/* Check it's an enum and check user has permission to ALTER the enum */
	checkEnumOwner(tup);

	ReleaseSysCache(tup);

	if (stmt->oldVal)
	{
		/* Rename an existing label */
		RenameEnumLabel(enum_type_oid, stmt->oldVal, stmt->newVal);
	}
	else
	{
		/* Add a new label */
		AddEnumLabel(enum_type_oid, stmt->newVal,
					 stmt->newValNeighbor, stmt->newValIsAfter,
					 stmt->skipIfNewValExists);
	}

	InvokeObjectPostAlterHook(TypeRelationId, enum_type_oid, 0);

	ObjectAddressSet(address, TypeRelationId, enum_type_oid);

	return address;
}


/*
 * checkEnumOwner
 *
 * Check that the type is actually an enum and that the current user
 * has permission to do ALTER TYPE on it.  Throw an error if not.
 */
static void
checkEnumOwner(HeapTuple tup)
{
	Form_pg_type typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check that this is actually an enum */
	if (typTup->typtype != TYPTYPE_ENUM)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not an enum",
						format_type_be(typTup->oid))));

	/* Permission check: must own type */
	if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);
}


/*
 * DefineRange
 *		Registers a new range type.
 *
 * Perhaps it might be worthwhile to set pg_type.typelem to the base type,
 * and likewise on multiranges to set it to the range type. But having a
 * non-zero typelem is treated elsewhere as a synonym for being an array,
 * and users might have queries with that same assumption.
 */
ObjectAddress
DefineRange(ParseState *pstate, CreateRangeStmt *stmt)
{
	char	   *typeName;
	Oid			typeNamespace;
	Oid			typoid;
	char	   *rangeArrayName;
	char	   *multirangeTypeName = NULL;
	char	   *multirangeArrayName;
	Oid			multirangeNamespace = InvalidOid;
	Oid			rangeArrayOid;
	Oid			multirangeOid;
	Oid			multirangeArrayOid;
	Oid			rangeSubtype = InvalidOid;
	List	   *rangeSubOpclassName = NIL;
	List	   *rangeCollationName = NIL;
	List	   *rangeCanonicalName = NIL;
	List	   *rangeSubtypeDiffName = NIL;
	Oid			rangeSubOpclass;
	Oid			rangeCollation;
	regproc		rangeCanonical;
	regproc		rangeSubtypeDiff;
	int16		subtyplen;
	bool		subtypbyval;
	char		subtypalign;
	char		alignment;
	AclResult	aclresult;
	ListCell   *lc;
	ObjectAddress address;
	ObjectAddress mltrngaddress PG_USED_FOR_ASSERTS_ONLY;
	Oid			castFuncOid;

	/* Convert list of names to a name and namespace */
	typeNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &typeName);

	/* Check we have creation rights in target namespace */
	aclresult = object_aclcheck(NamespaceRelationId, typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));

	/*
	 * Look to see if type already exists.
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/*
	 * Unlike DefineType(), we don't insist on a shell type existing first, as
	 * it's only needed if the user wants to specify a canonical function.
	 */

	/* Extract the parameters from the parameter list */
	foreach(lc, stmt->params)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "subtype") == 0)
		{
			if (OidIsValid(rangeSubtype))
				errorConflictingDefElem(defel, pstate);
			/* we can look up the subtype name immediately */
			rangeSubtype = typenameTypeId(NULL, defGetTypeName(defel));
		}
		else if (strcmp(defel->defname, "subtype_opclass") == 0)
		{
			if (rangeSubOpclassName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeSubOpclassName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "collation") == 0)
		{
			if (rangeCollationName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeCollationName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "canonical") == 0)
		{
			if (rangeCanonicalName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeCanonicalName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "subtype_diff") == 0)
		{
			if (rangeSubtypeDiffName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeSubtypeDiffName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "multirange_type_name") == 0)
		{
			if (multirangeTypeName != NULL)
				errorConflictingDefElem(defel, pstate);
			/* we can look up the subtype name immediately */
			multirangeNamespace = QualifiedNameGetCreationNamespace(defGetQualifiedName(defel),
																	&multirangeTypeName);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* Must have a subtype */
	if (!OidIsValid(rangeSubtype))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type attribute \"subtype\" is required")));
	/* disallow ranges of pseudotypes */
	if (get_typtype(rangeSubtype) == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("range subtype cannot be %s",
						format_type_be(rangeSubtype))));

	/* Identify subopclass */
	rangeSubOpclass = findRangeSubOpclass(rangeSubOpclassName, rangeSubtype);

	/* Identify collation to use, if any */
	if (type_is_collatable(rangeSubtype))
	{
		if (rangeCollationName != NIL)
			rangeCollation = get_collation_oid(rangeCollationName, false);
		else
			rangeCollation = get_typcollation(rangeSubtype);
	}
	else
	{
		if (rangeCollationName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("range collation specified but subtype does not support collation")));
		rangeCollation = InvalidOid;
	}

	/* Identify support functions, if provided */
	if (rangeCanonicalName != NIL)
	{
		if (!OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot specify a canonical function without a pre-created shell type"),
					 errhint("Create the type as a shell type, then create its canonicalization function, then do a full CREATE TYPE.")));
		rangeCanonical = findRangeCanonicalFunction(rangeCanonicalName,
													typoid);
	}
	else
		rangeCanonical = InvalidOid;

	if (rangeSubtypeDiffName != NIL)
		rangeSubtypeDiff = findRangeSubtypeDiffFunction(rangeSubtypeDiffName,
														rangeSubtype);
	else
		rangeSubtypeDiff = InvalidOid;

	get_typlenbyvalalign(rangeSubtype,
						 &subtyplen, &subtypbyval, &subtypalign);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for ranges */
	alignment = (subtypalign == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	/* Allocate OID for array type, its multirange, and its multirange array */
	rangeArrayOid = AssignTypeArrayOid();
	multirangeOid = AssignTypeMultirangeOid();
	multirangeArrayOid = AssignTypeMultirangeArrayOid();

	/* Create the pg_type entry */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
				   typeName,	/* type name */
				   typeNamespace,	/* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   -1,			/* internal size (always varlena) */
				   TYPTYPE_RANGE,	/* type-type (range type) */
				   TYPCATEGORY_RANGE,	/* type-category (range type) */
				   false,		/* range types are never preferred */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   F_RANGE_IN,	/* input procedure */
				   F_RANGE_OUT, /* output procedure */
				   F_RANGE_RECV,	/* receive procedure */
				   F_RANGE_SEND,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   F_RANGE_TYPANALYZE,	/* analyze procedure */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* element type ID - none */
				   false,		/* this is not an array type */
				   rangeArrayOid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* never a default type value */
				   NULL,		/* no binary form available either */
				   false,		/* never passed by value */
				   alignment,	/* alignment */
				   TYPSTORAGE_EXTENDED, /* TOAST strategy (always extended) */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation (ranges never have one) */
	Assert(typoid == InvalidOid || typoid == address.objectId);
	typoid = address.objectId;

	/* Create the multirange that goes with it */
	if (multirangeTypeName)
	{
		Oid			old_typoid;

		/*
		 * Look to see if multirange type already exists.
		 */
		old_typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
									 CStringGetDatum(multirangeTypeName),
									 ObjectIdGetDatum(multirangeNamespace));

		/*
		 * If it's not a shell, see if it's an autogenerated array type, and
		 * if so rename it out of the way.
		 */
		if (OidIsValid(old_typoid) && get_typisdefined(old_typoid))
		{
			if (!moveArrayTypeName(old_typoid, multirangeTypeName, multirangeNamespace))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("type \"%s\" already exists", multirangeTypeName)));
		}
	}
	else
	{
		/* Generate multirange name automatically */
		multirangeNamespace = typeNamespace;
		multirangeTypeName = makeMultirangeTypeName(typeName, multirangeNamespace);
	}

	mltrngaddress =
		TypeCreate(multirangeOid,	/* force assignment of this type OID */
				   multirangeTypeName,	/* type name */
				   multirangeNamespace, /* namespace */
				   InvalidOid,	/* relation oid (n/a here) */
				   0,			/* relation kind (ditto) */
				   GetUserId(), /* owner's ID */
				   -1,			/* internal size (always varlena) */
				   TYPTYPE_MULTIRANGE,	/* type-type (multirange type) */
				   TYPCATEGORY_RANGE,	/* type-category (range type) */
				   false,		/* multirange types are never preferred */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
				   F_MULTIRANGE_IN, /* input procedure */
				   F_MULTIRANGE_OUT,	/* output procedure */
				   F_MULTIRANGE_RECV,	/* receive procedure */
				   F_MULTIRANGE_SEND,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   F_MULTIRANGE_TYPANALYZE, /* analyze procedure */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* element type ID - none */
				   false,		/* this is not an array type */
				   multirangeArrayOid,	/* array type we are about to create */
				   InvalidOid,	/* base type ID (only for domains) */
				   NULL,		/* never a default type value */
				   NULL,		/* no binary form available either */
				   false,		/* never passed by value */
				   alignment,	/* alignment */
				   'x',			/* TOAST strategy (always extended) */
				   -1,			/* typMod (Domains only) */
				   0,			/* Array dimensions of typbasetype */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* type's collation (ranges never have one) */
	Assert(multirangeOid == mltrngaddress.objectId);

	/* Create the entry in pg_range */
	RangeCreate(typoid, rangeSubtype, rangeCollation, rangeSubOpclass,
				rangeCanonical, rangeSubtypeDiff, multirangeOid);

	/*
	 * Create the array type that goes with it.
	 */
	rangeArrayName = makeArrayTypeName(typeName, typeNamespace);

	TypeCreate(rangeArrayOid,	/* force assignment of this type OID */
			   rangeArrayName,	/* type name */
			   typeNamespace,	/* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure - none */
			   InvalidOid,		/* typmodout procedure - none */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   typoid,			/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* alignment - same as range's */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* typcollation */

	pfree(rangeArrayName);

	/* Create the multirange's array type */

	multirangeArrayName = makeArrayTypeName(multirangeTypeName, typeNamespace);

	TypeCreate(multirangeArrayOid,	/* force assignment of this type OID */
			   multirangeArrayName, /* type name */
			   multirangeNamespace, /* namespace */
			   InvalidOid,		/* relation oid (n/a here) */
			   0,				/* relation kind (ditto) */
			   GetUserId(),		/* owner's ID */
			   -1,				/* internal size (always varlena) */
			   TYPTYPE_BASE,	/* type-type (base type) */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
			   false,			/* array types are never preferred */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
			   F_ARRAY_IN,		/* input procedure */
			   F_ARRAY_OUT,		/* output procedure */
			   F_ARRAY_RECV,	/* receive procedure */
			   F_ARRAY_SEND,	/* send procedure */
			   InvalidOid,		/* typmodin procedure - none */
			   InvalidOid,		/* typmodout procedure - none */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
			   multirangeOid,	/* element type ID */
			   true,			/* yes this is an array type */
			   InvalidOid,		/* no further array type */
			   InvalidOid,		/* base type ID */
			   NULL,			/* never a default type value */
			   NULL,			/* binary default isn't sent either */
			   false,			/* never passed by value */
			   alignment,		/* alignment - same as range's */
			   'x',				/* ARRAY is always toastable */
			   -1,				/* typMod (Domains only) */
			   0,				/* Array dimensions of typbasetype */
			   false,			/* Type NOT NULL */
			   InvalidOid);		/* typcollation */

	/* And create the constructor functions for this range type */
	makeRangeConstructors(typeName, typeNamespace, typoid, rangeSubtype);
	makeMultirangeConstructors(multirangeTypeName, typeNamespace,
							   multirangeOid, typoid, rangeArrayOid,
							   &castFuncOid);

	/* Create cast from the range type to its multirange type */
	CastCreate(typoid, multirangeOid, castFuncOid, InvalidOid, InvalidOid,
			   COERCION_CODE_EXPLICIT, COERCION_METHOD_FUNCTION,
			   DEPENDENCY_INTERNAL);

	pfree(multirangeArrayName);

	return address;
}

/*
 * Because there may exist several range types over the same subtype, the
 * range type can't be uniquely determined from the subtype.  So it's
 * impossible to define a polymorphic constructor; we have to generate new
 * constructor functions explicitly for each range type.
 *
 * We actually define 4 functions, with 0 through 3 arguments.  This is just
 * to offer more convenience for the user.
 */
static void
makeRangeConstructors(const char *name, Oid namespace,
					  Oid rangeOid, Oid subtype)
{
	static const char *const prosrc[2] = {"range_constructor2",
	"range_constructor3"};
	static const int pronargs[2] = {2, 3};

	Oid			constructorArgTypes[3];
	ObjectAddress myself,
				referenced;
	int			i;

	constructorArgTypes[0] = subtype;
	constructorArgTypes[1] = subtype;
	constructorArgTypes[2] = TEXTOID;

	referenced.classId = TypeRelationId;
	referenced.objectId = rangeOid;
	referenced.objectSubId = 0;

	for (i = 0; i < lengthof(prosrc); i++)
	{
		oidvector  *constructorArgTypesVector;

		constructorArgTypesVector = buildoidvector(constructorArgTypes,
												   pronargs[i]);

		myself = ProcedureCreate(name,	/* name: same as range type */
								 namespace, /* namespace */
								 false, /* replace */
								 false, /* returns set */
								 rangeOid,	/* return type */
								 BOOTSTRAP_SUPERUSERID, /* proowner */
								 INTERNALlanguageId,	/* language */
								 F_FMGR_INTERNAL_VALIDATOR, /* language validator */
								 prosrc[i], /* prosrc */
								 NULL,	/* probin */
								 NULL,	/* prosqlbody */
								 PROKIND_FUNCTION,
								 false, /* security_definer */
								 false, /* leakproof */
								 false, /* isStrict */
								 PROVOLATILE_IMMUTABLE, /* volatility */
								 PROPARALLEL_SAFE,	/* parallel safety */
								 constructorArgTypesVector, /* parameterTypes */
								 PointerGetDatum(NULL), /* allParameterTypes */
								 PointerGetDatum(NULL), /* parameterModes */
								 PointerGetDatum(NULL), /* parameterNames */
								 NIL,	/* parameterDefaults */
								 PointerGetDatum(NULL), /* trftypes */
								 PointerGetDatum(NULL), /* proconfig */
								 InvalidOid,	/* prosupport */
								 1.0,	/* procost */
								 0.0);	/* prorows */

		/*
		 * Make the constructors internally-dependent on the range type so
		 * that they go away silently when the type is dropped.  Note that
		 * pg_dump depends on this choice to avoid dumping the constructors.
		 */
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
}

/*
 * We make a separate multirange constructor for each range type
 * so its name can include the base type, like range constructors do.
 * If we had an anyrangearray polymorphic type we could use it here,
 * but since each type has its own constructor name there's no need.
 *
 * Sets castFuncOid to the oid of the new constructor that can be used
 * to cast from a range to a multirange.
 */
static void
makeMultirangeConstructors(const char *name, Oid namespace,
						   Oid multirangeOid, Oid rangeOid, Oid rangeArrayOid,
						   Oid *castFuncOid)
{
	ObjectAddress myself,
				referenced;
	oidvector  *argtypes;
	Datum		allParamTypes;
	ArrayType  *allParameterTypes;
	Datum		paramModes;
	ArrayType  *parameterModes;

	referenced.classId = TypeRelationId;
	referenced.objectId = multirangeOid;
	referenced.objectSubId = 0;

	/* 0-arg constructor - for empty multiranges */
	argtypes = buildoidvector(NULL, 0);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
							 namespace,
							 false, /* replace */
							 false, /* returns set */
							 multirangeOid, /* return type */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
							 INTERNALlanguageId,	/* language */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor0", /* prosrc */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* leakproof */
							 true,	/* isStrict */
							 PROVOLATILE_IMMUTABLE, /* volatility */
							 PROPARALLEL_SAFE,	/* parallel safety */
							 argtypes,	/* parameterTypes */
							 PointerGetDatum(NULL), /* allParameterTypes */
							 PointerGetDatum(NULL), /* parameterModes */
							 PointerGetDatum(NULL), /* parameterNames */
							 NIL,	/* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* prosupport */
							 1.0,	/* procost */
							 0.0);	/* prorows */

	/*
	 * Make the constructor internally-dependent on the multirange type so
	 * that they go away silently when the type is dropped.  Note that pg_dump
	 * depends on this choice to avoid dumping the constructors.
	 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);

	/*
	 * 1-arg constructor - for casts
	 *
	 * In theory we shouldn't need both this and the vararg (n-arg)
	 * constructor, but having a separate 1-arg function lets us define casts
	 * against it.
	 */
	argtypes = buildoidvector(&rangeOid, 1);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
							 namespace,
							 false, /* replace */
							 false, /* returns set */
							 multirangeOid, /* return type */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
							 INTERNALlanguageId,	/* language */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor1", /* prosrc */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* leakproof */
							 true,	/* isStrict */
							 PROVOLATILE_IMMUTABLE, /* volatility */
							 PROPARALLEL_SAFE,	/* parallel safety */
							 argtypes,	/* parameterTypes */
							 PointerGetDatum(NULL), /* allParameterTypes */
							 PointerGetDatum(NULL), /* parameterModes */
							 PointerGetDatum(NULL), /* parameterNames */
							 NIL,	/* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* prosupport */
							 1.0,	/* procost */
							 0.0);	/* prorows */
	/* ditto */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	*castFuncOid = myself.objectId;

	/* n-arg constructor - vararg */
	argtypes = buildoidvector(&rangeArrayOid, 1);
	allParamTypes = ObjectIdGetDatum(rangeArrayOid);
	allParameterTypes = construct_array_builtin(&allParamTypes, 1, OIDOID);
	paramModes = CharGetDatum(FUNC_PARAM_VARIADIC);
	parameterModes = construct_array_builtin(&paramModes, 1, CHAROID);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
							 namespace,
							 false, /* replace */
							 false, /* returns set */
							 multirangeOid, /* return type */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
							 INTERNALlanguageId,	/* language */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor2", /* prosrc */
							 NULL,	/* probin */
							 NULL,	/* prosqlbody */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* leakproof */
							 true,	/* isStrict */
							 PROVOLATILE_IMMUTABLE, /* volatility */
							 PROPARALLEL_SAFE,	/* parallel safety */
							 argtypes,	/* parameterTypes */
							 PointerGetDatum(allParameterTypes),	/* allParameterTypes */
							 PointerGetDatum(parameterModes),	/* parameterModes */
							 PointerGetDatum(NULL), /* parameterNames */
							 NIL,	/* parameterDefaults */
							 PointerGetDatum(NULL), /* trftypes */
							 PointerGetDatum(NULL), /* proconfig */
							 InvalidOid,	/* prosupport */
							 1.0,	/* procost */
							 0.0);	/* prorows */
	/* ditto */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	pfree(allParameterTypes);
	pfree(parameterModes);
}

/*
 * Find suitable I/O and other support functions for a type.
 *
 * typeOid is the type's OID (which will already exist, if only as a shell
 * type).
 */

static Oid
findTypeInputFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/*
	 * Input functions can take a single argument of type CSTRING, or three
	 * arguments (string, typioparam OID, typmod).  Whine about ambiguity if
	 * both forms exist.
	 */
	argList[0] = CSTRINGOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type input function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* If not found, reference the 1-argument signature in error msg */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* Input functions must return the target type. */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/*
	 * Print warnings if any of the type's I/O functions are marked volatile.
	 * There is a general assumption that I/O functions are stable or
	 * immutable; this allows us for example to mark record_in/record_out
	 * stable rather than volatile.  Ideally we would throw errors not just
	 * warnings here; but since this check is new as of 9.5, and since the
	 * volatility marking might be just an error-of-omission and not a true
	 * indication of how the function behaves, we'll let it pass as a warning
	 * for now.
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeOutputFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Output functions always take a single argument of the type and return
	 * cstring.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeReceiveFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/*
	 * Receive functions can take a single argument of type INTERNAL, or three
	 * arguments (internal, typioparam OID, typmod).  Whine about ambiguity if
	 * both forms exist.
	 */
	argList[0] = INTERNALOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type receive function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* If not found, reference the 1-argument signature in error msg */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* Receive functions must return the target type. */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeSendFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Send functions always take a single argument of the type and return
	 * bytea.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s must return type %s",
						NameListToString(procname), "bytea")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeTypmodinFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * typmodin functions always take one cstring[] argument and return int4.
	 */
	argList[0] = CSTRINGARRAYOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_in function %s must return type %s",
						NameListToString(procname), "integer")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeTypmodoutFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * typmodout functions always take one int4 argument and return cstring.
	 */
	argList[0] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_out function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeAnalyzeFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Analyze functions always take one INTERNAL argument and return bool.
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type analyze function %s must return type %s",
						NameListToString(procname), "boolean")));

	return procOid;
}

static Oid
findTypeSubscriptingFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Subscripting support functions always take one INTERNAL argument and
	 * return INTERNAL.  (The argument is not used, but we must have it to
	 * maintain type safety.)
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type subscripting function %s must return type %s",
						NameListToString(procname), "internal")));

	/*
	 * We disallow array_subscript_handler() from being selected explicitly,
	 * since that must only be applied to autogenerated array types.
	 */
	if (procOid == F_ARRAY_SUBSCRIPT_HANDLER)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("user-defined types cannot use subscripting function %s",
						NameListToString(procname))));

	return procOid;
}

/*
 * Find suitable support functions and opclasses for a range type.
 */

/*
 * Find named btree opclass for subtype, or default btree opclass if
 * opcname is NIL.
 */
static Oid
findRangeSubOpclass(List *opcname, Oid subtype)
{
	Oid			opcid;
	Oid			opInputType;

	if (opcname != NIL)
	{
		opcid = get_opclass_oid(BTREE_AM_OID, opcname, false);

		/*
		 * Verify that the operator class accepts this datatype. Note we will
		 * accept binary compatibility.
		 */
		opInputType = get_opclass_input_type(opcid);
		if (!IsBinaryCoercible(subtype, opInputType))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("operator class \"%s\" does not accept data type %s",
							NameListToString(opcname),
							format_type_be(subtype))));
	}
	else
	{
		opcid = GetDefaultOpClass(subtype, BTREE_AM_OID);
		if (!OidIsValid(opcid))
		{
			/* We spell the error message identically to ResolveOpClass */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(subtype), "btree"),
					 errhint("You must specify an operator class for the range type or define a default operator class for the subtype.")));
		}
	}

	return opcid;
}

static Oid
findRangeCanonicalFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;
	AclResult	aclresult;

	/*
	 * Range canonical functions must take and return the range type, and must
	 * be immutable.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must return range type",
						func_signature_string(procname, 1, NIL, argList))));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must be immutable",
						func_signature_string(procname, 1, NIL, argList))));

	/* Also, range type's creator must have permission to call function */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(procOid));

	return procOid;
}

static Oid
findRangeSubtypeDiffFunction(List *procname, Oid subtype)
{
	Oid			argList[2];
	Oid			procOid;
	AclResult	aclresult;

	/*
	 * Range subtype diff functions must take two arguments of the subtype,
	 * must return float8, and must be immutable.
	 */
	argList[0] = subtype;
	argList[1] = subtype;

	procOid = LookupFuncName(procname, 2, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 2, NIL, argList))));

	if (get_func_rettype(procOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must return type %s",
						func_signature_string(procname, 2, NIL, argList),
						"double precision")));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must be immutable",
						func_signature_string(procname, 2, NIL, argList))));

	/* Also, range type's creator must have permission to call function */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(procOid));

	return procOid;
}

/*
 *	AssignTypeArrayOid
 *
 *	Pre-assign the type's array OID for use in pg_type.typarray
 */
Oid
AssignTypeArrayOid(void)
{
	Oid			type_array_oid;

	/* Use binary-upgrade override for pg_type.typarray? */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_array_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type array OID value not set when in binary upgrade mode")));

		type_array_oid = binary_upgrade_next_array_pg_type_oid;
		binary_upgrade_next_array_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
											Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_array_oid;
}

/*
 *	AssignTypeMultirangeOid
 *
 *	Pre-assign the range type's multirange OID for use in pg_type.oid
 */
Oid
AssignTypeMultirangeOid(void)
{
	Oid			type_multirange_oid;

	/* Use binary-upgrade override for pg_type.oid? */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_mrng_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type multirange OID value not set when in binary upgrade mode")));

		type_multirange_oid = binary_upgrade_next_mrng_pg_type_oid;
		binary_upgrade_next_mrng_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
												 Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_oid;
}

/*
 *	AssignTypeMultirangeArrayOid
 *
 *	Pre-assign the range type's multirange array OID for use in pg_type.typarray
 */
Oid
AssignTypeMultirangeArrayOid(void)
{
	Oid			type_multirange_array_oid;

	/* Use binary-upgrade override for pg_type.oid? */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_mrng_array_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type multirange array OID value not set when in binary upgrade mode")));

		type_multirange_array_oid = binary_upgrade_next_mrng_array_pg_type_oid;
		binary_upgrade_next_mrng_array_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
													   Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_array_oid;
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
 * Return type is the new type's object address.
 *-------------------------------------------------------------------
 */
ObjectAddress
DefineCompositeType(RangeVar *typevar, List *coldeflist)
{
	CreateStmt *createStmt = makeNode(CreateStmt);
	Oid			old_type_oid;
	Oid			typeNamespace;
	ObjectAddress address;

	/*
	 * now set the parameters for keys/inheritance etc. All of these are
	 * uninteresting for composite types...
	 */
	createStmt->relation = typevar;
	createStmt->tableElts = coldeflist;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->options = NIL;
	createStmt->oncommit = ONCOMMIT_NOOP;
	createStmt->tablespacename = NULL;
	createStmt->if_not_exists = false;

	/*
	 * Check for collision with an existing type name. If there is one and
	 * it's an autogenerated array, we can rename it out of the way.  This
	 * check is here mainly to get a better error message about a "type"
	 * instead of below about a "relation".
	 */
	typeNamespace = RangeVarGetAndCheckCreationNamespace(createStmt->relation,
														 NoLock, NULL);
	RangeVarAdjustRelationPersistence(createStmt->relation, typeNamespace);
	old_type_oid =
		GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						CStringGetDatum(createStmt->relation->relname),
						ObjectIdGetDatum(typeNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, createStmt->relation->relname, typeNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", createStmt->relation->relname)));
	}

	/*
	 * Finally create the relation.  This also creates the type.
	 */
	DefineRelation(createStmt, RELKIND_COMPOSITE_TYPE, InvalidOid, &address,
				   NULL);

	return address;
}

/*
 * AlterDomainDefault
 *
 * Routine implementing ALTER DOMAIN SET/DROP DEFAULT statements.
 *
 * Returns ObjectAddress of the modified domain.
 */
ObjectAddress
AlterDomainDefault(List *names, Node *defaultRaw)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	ParseState *pstate;
	Relation	rel;
	char	   *defaultValue;
	Node	   *defaultExpr = NULL; /* NULL if no default specified */
	Datum		new_record[Natts_pg_type] = {0};
	bool		new_record_nulls[Natts_pg_type] = {0};
	bool		new_record_repl[Natts_pg_type] = {0};
	HeapTuple	newtuple;
	Form_pg_type typTup;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	checkDomainOwner(tup);

	/* Setup new tuple */

	/* Store the new default into the tuple */
	if (defaultRaw)
	{
		/* Create a dummy ParseState for transformExpr */
		pstate = make_parsestate(NULL);

		/*
		 * Cook the colDef->raw_expr into an expression. Note: Name is
		 * strictly for error message
		 */
		defaultExpr = cookDefault(pstate, defaultRaw,
								  typTup->typbasetype,
								  typTup->typtypmod,
								  NameStr(typTup->typname),
								  0);

		/*
		 * If the expression is just a NULL constant, we treat the command
		 * like ALTER ... DROP DEFAULT.  (But see note for same test in
		 * DefineDomain.)
		 */
		if (defaultExpr == NULL ||
			(IsA(defaultExpr, Const) && ((Const *) defaultExpr)->constisnull))
		{
			/* Default is NULL, drop it */
			defaultExpr = NULL;
			new_record_nulls[Anum_pg_type_typdefaultbin - 1] = true;
			new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
			new_record_nulls[Anum_pg_type_typdefault - 1] = true;
			new_record_repl[Anum_pg_type_typdefault - 1] = true;
		}
		else
		{
			/*
			 * Expression must be stored as a nodeToString result, but we also
			 * require a valid textual representation (mainly to make life
			 * easier for pg_dump).
			 */
			defaultValue = deparse_expression(defaultExpr,
											  NIL, false, false);

			/*
			 * Form an updated tuple with the new default and write it back.
			 */
			new_record[Anum_pg_type_typdefaultbin - 1] = CStringGetTextDatum(nodeToString(defaultExpr));

			new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
			new_record[Anum_pg_type_typdefault - 1] = CStringGetTextDatum(defaultValue);
			new_record_repl[Anum_pg_type_typdefault - 1] = true;
		}
	}
	else
	{
		/* ALTER ... DROP DEFAULT */
		new_record_nulls[Anum_pg_type_typdefaultbin - 1] = true;
		new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
		new_record_nulls[Anum_pg_type_typdefault - 1] = true;
		new_record_repl[Anum_pg_type_typdefault - 1] = true;
	}

	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel),
								 new_record, new_record_nulls,
								 new_record_repl);

	CatalogTupleUpdate(rel, &tup->t_self, newtuple);

	/* Rebuild dependencies */
	GenerateTypeDependencies(newtuple,
							 rel,
							 defaultExpr,
							 NULL,	/* don't have typacl handy */
							 0, /* relation kind is n/a */
							 false, /* a domain isn't an implicit array */
							 false, /* nor is it any kind of dependent type */
							 false, /* don't touch extension membership */
							 true); /* We do need to rebuild dependencies */

	InvokeObjectPostAlterHook(TypeRelationId, domainoid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	table_close(rel, RowExclusiveLock);
	heap_freetuple(newtuple);

	return address;
}

/*
 * AlterDomainNotNull
 *
 * Routine implementing ALTER DOMAIN SET/DROP NOT NULL statements.
 *
 * Returns ObjectAddress of the modified domain.
 */
ObjectAddress
AlterDomainNotNull(List *names, bool notNull)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	HeapTuple	tup;
	Form_pg_type typTup;
	ObjectAddress address = InvalidObjectAddress;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	typrel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	checkDomainOwner(tup);

	/* Is the domain already set to the desired constraint? */
	if (typTup->typnotnull == notNull)
	{
		table_close(typrel, RowExclusiveLock);
		return address;
	}

	if (notNull)
	{
		Constraint *constr;

		constr = makeNode(Constraint);
		constr->contype = CONSTR_NOTNULL;
		constr->initially_valid = true;
		constr->location = -1;

		domainAddNotNullConstraint(domainoid, typTup->typnamespace,
								   typTup->typbasetype, typTup->typtypmod,
								   constr, NameStr(typTup->typname), NULL);

		validateDomainNotNullConstraint(domainoid);
	}
	else
	{
		HeapTuple	conTup;
		ObjectAddress conobj;

		conTup = findDomainNotNullConstraint(domainoid);
		if (conTup == NULL)
			elog(ERROR, "could not find not-null constraint on domain \"%s\"", NameStr(typTup->typname));

		ObjectAddressSet(conobj, ConstraintRelationId, ((Form_pg_constraint) GETSTRUCT(conTup))->oid);
		performDeletion(&conobj, DROP_RESTRICT, 0);
	}

	/*
	 * Okay to update pg_type row.  We can scribble on typTup because it's a
	 * copy.
	 */
	typTup->typnotnull = notNull;

	CatalogTupleUpdate(typrel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(TypeRelationId, domainoid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	heap_freetuple(tup);
	table_close(typrel, RowExclusiveLock);

	return address;
}

/*
 * AlterDomainDropConstraint
 *
 * Implements the ALTER DOMAIN DROP CONSTRAINT statement
 *
 * Returns ObjectAddress of the modified domain.
 */
ObjectAddress
AlterDomainDropConstraint(List *names, const char *constrName,
						  DropBehavior behavior, bool missing_ok)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	Relation	rel;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey[3];
	HeapTuple	contup;
	bool		found = false;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	checkDomainOwner(tup);

	/* Grab an appropriate lock on the pg_constraint relation */
	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/* Find and remove the target constraint */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainoid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));

	conscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 3, skey);

	/* There can be at most one matching row */
	if ((contup = systable_getnext(conscan)) != NULL)
	{
		Form_pg_constraint construct = (Form_pg_constraint) GETSTRUCT(contup);
		ObjectAddress conobj;

		if (construct->contype == CONSTRAINT_NOTNULL)
		{
			((Form_pg_type) GETSTRUCT(tup))->typnotnull = false;
			CatalogTupleUpdate(rel, &tup->t_self, tup);
		}

		conobj.classId = ConstraintRelationId;
		conobj.objectId = construct->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);
		found = true;
	}

	/* Clean up after the scan */
	systable_endscan(conscan);
	table_close(conrel, RowExclusiveLock);

	if (!found)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of domain \"%s\" does not exist",
							constrName, TypeNameToString(typename))));
		else
			ereport(NOTICE,
					(errmsg("constraint \"%s\" of domain \"%s\" does not exist, skipping",
							constrName, TypeNameToString(typename))));
	}

	/*
	 * We must send out an sinval message for the domain, to ensure that any
	 * dependent plans get rebuilt.  Since this command doesn't change the
	 * domain's pg_type row, that won't happen automatically; do it manually.
	 */
	CacheInvalidateHeapTuple(rel, tup, NULL);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * AlterDomainAddConstraint
 *
 * Implements the ALTER DOMAIN .. ADD CONSTRAINT statement.
 */
ObjectAddress
AlterDomainAddConstraint(List *names, Node *newConstraint,
						 ObjectAddress *constrAddr)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	HeapTuple	tup;
	Form_pg_type typTup;
	Constraint *constr;
	char	   *ccbin;
	ObjectAddress address = InvalidObjectAddress;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	typrel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	checkDomainOwner(tup);

	if (!IsA(newConstraint, Constraint))
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(newConstraint));

	constr = (Constraint *) newConstraint;

	switch (constr->contype)
	{
		case CONSTR_CHECK:
		case CONSTR_NOTNULL:
			/* processed below */
			break;

		case CONSTR_UNIQUE:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unique constraints not possible for domains")));
			break;

		case CONSTR_PRIMARY:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("primary key constraints not possible for domains")));
			break;

		case CONSTR_EXCLUSION:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("exclusion constraints not possible for domains")));
			break;

		case CONSTR_FOREIGN:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("foreign key constraints not possible for domains")));
			break;

		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("specifying constraint deferrability not supported for domains")));
			break;

		default:
			elog(ERROR, "unrecognized constraint subtype: %d",
				 (int) constr->contype);
			break;
	}

	if (constr->contype == CONSTR_CHECK)
	{
		/*
		 * First, process the constraint expression and add an entry to
		 * pg_constraint.
		 */

		ccbin = domainAddCheckConstraint(domainoid, typTup->typnamespace,
										 typTup->typbasetype, typTup->typtypmod,
										 constr, NameStr(typTup->typname), constrAddr);


		/*
		 * If requested to validate the constraint, test all values stored in
		 * the attributes based on the domain the constraint is being added
		 * to.
		 */
		if (!constr->skip_validation)
			validateDomainCheckConstraint(domainoid, ccbin);

		/*
		 * We must send out an sinval message for the domain, to ensure that
		 * any dependent plans get rebuilt.  Since this command doesn't change
		 * the domain's pg_type row, that won't happen automatically; do it
		 * manually.
		 */
		CacheInvalidateHeapTuple(typrel, tup, NULL);
	}
	else if (constr->contype == CONSTR_NOTNULL)
	{
		/* Is the domain already set NOT NULL? */
		if (typTup->typnotnull)
		{
			table_close(typrel, RowExclusiveLock);
			return address;
		}
		domainAddNotNullConstraint(domainoid, typTup->typnamespace,
								   typTup->typbasetype, typTup->typtypmod,
								   constr, NameStr(typTup->typname), constrAddr);

		if (!constr->skip_validation)
			validateDomainNotNullConstraint(domainoid);

		typTup->typnotnull = true;
		CatalogTupleUpdate(typrel, &tup->t_self, tup);
	}

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	table_close(typrel, RowExclusiveLock);

	return address;
}

/*
 * AlterDomainValidateConstraint
 *
 * Implements the ALTER DOMAIN .. VALIDATE CONSTRAINT statement.
 */
ObjectAddress
AlterDomainValidateConstraint(List *names, const char *constrName)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	Relation	conrel;
	HeapTuple	tup;
	Form_pg_constraint con;
	Form_pg_constraint copy_con;
	char	   *conbin;
	SysScanDesc scan;
	Datum		val;
	HeapTuple	tuple;
	HeapTuple	copyTuple;
	ScanKeyData skey[3];
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	typrel = table_open(TypeRelationId, AccessShareLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	checkDomainOwner(tup);

	/*
	 * Find and check the target constraint
	 */
	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainoid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));

	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* There can be at most one matching row */
	if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of domain \"%s\" does not exist",
						constrName, TypeNameToString(typename))));

	con = (Form_pg_constraint) GETSTRUCT(tuple);
	if (con->contype != CONSTRAINT_CHECK)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of domain \"%s\" is not a check constraint",
						constrName, TypeNameToString(typename))));

	val = SysCacheGetAttrNotNull(CONSTROID, tuple, Anum_pg_constraint_conbin);
	conbin = TextDatumGetCString(val);

	validateDomainCheckConstraint(domainoid, conbin);

	/*
	 * Now update the catalog, while we have the door open.
	 */
	copyTuple = heap_copytuple(tuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
	copy_con->convalidated = true;
	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	heap_freetuple(copyTuple);

	systable_endscan(scan);

	table_close(typrel, AccessShareLock);
	table_close(conrel, RowExclusiveLock);

	ReleaseSysCache(tup);

	return address;
}

/*
 * Verify that all columns currently using the domain are not null.
 */
static void
validateDomainNotNullConstraint(Oid domainoid)
{
	List	   *rels;
	ListCell   *rt;

	/* Fetch relation list with attributes based on this domain */
	/* ShareLock is sufficient to prevent concurrent data changes */

	rels = get_rels_with_domain(domainoid, ShareLock);

	foreach(rt, rels)
	{
		RelToCheck *rtc = (RelToCheck *) lfirst(rt);
		Relation	testrel = rtc->rel;
		TupleDesc	tupdesc = RelationGetDescr(testrel);
		TupleTableSlot *slot;
		TableScanDesc scan;
		Snapshot	snapshot;

		/* Scan all tuples in this relation */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(testrel, snapshot, 0, NULL);
		slot = table_slot_create(testrel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			int			i;

			/* Test attributes that are of the domain */
			for (i = 0; i < rtc->natts; i++)
			{
				int			attnum = rtc->atts[i];
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

				if (slot_attisnull(slot, attnum))
				{
					/*
					 * In principle the auxiliary information for this error
					 * should be errdatatype(), but errtablecol() seems
					 * considerably more useful in practice.  Since this code
					 * only executes in an ALTER DOMAIN command, the client
					 * should already know which domain is in question.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("column \"%s\" of table \"%s\" contains null values",
									NameStr(attr->attname),
									RelationGetRelationName(testrel)),
							 errtablecol(testrel, attnum)));
				}
			}
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		/* Close each rel after processing, but keep lock */
		table_close(testrel, NoLock);
	}
}

/*
 * Verify that all columns currently using the domain satisfy the given check
 * constraint expression.
 */
static void
validateDomainCheckConstraint(Oid domainoid, const char *ccbin)
{
	Expr	   *expr = (Expr *) stringToNode(ccbin);
	List	   *rels;
	ListCell   *rt;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *exprstate;

	/* Need an EState to run ExecEvalExpr */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);

	/* build execution state for expr */
	exprstate = ExecPrepareExpr(expr, estate);

	/* Fetch relation list with attributes based on this domain */
	/* ShareLock is sufficient to prevent concurrent data changes */

	rels = get_rels_with_domain(domainoid, ShareLock);

	foreach(rt, rels)
	{
		RelToCheck *rtc = (RelToCheck *) lfirst(rt);
		Relation	testrel = rtc->rel;
		TupleDesc	tupdesc = RelationGetDescr(testrel);
		TupleTableSlot *slot;
		TableScanDesc scan;
		Snapshot	snapshot;

		/* Scan all tuples in this relation */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(testrel, snapshot, 0, NULL);
		slot = table_slot_create(testrel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			int			i;

			/* Test attributes that are of the domain */
			for (i = 0; i < rtc->natts; i++)
			{
				int			attnum = rtc->atts[i];
				Datum		d;
				bool		isNull;
				Datum		conResult;
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

				d = slot_getattr(slot, attnum, &isNull);

				econtext->domainValue_datum = d;
				econtext->domainValue_isNull = isNull;

				conResult = ExecEvalExprSwitchContext(exprstate,
													  econtext,
													  &isNull);

				if (!isNull && !DatumGetBool(conResult))
				{
					/*
					 * In principle the auxiliary information for this error
					 * should be errdomainconstraint(), but errtablecol()
					 * seems considerably more useful in practice.  Since this
					 * code only executes in an ALTER DOMAIN command, the
					 * client should already know which domain is in question,
					 * and which constraint too.
					 */
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("column \"%s\" of table \"%s\" contains values that violate the new constraint",
									NameStr(attr->attname),
									RelationGetRelationName(testrel)),
							 errtablecol(testrel, attnum)));
				}
			}

			ResetExprContext(econtext);
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		/* Hold relation lock till commit (XXX bad for concurrency) */
		table_close(testrel, NoLock);
	}

	FreeExecutorState(estate);
}

/*
 * get_rels_with_domain
 *
 * Fetch all relations / attributes which are using the domain
 *
 * The result is a list of RelToCheck structs, one for each distinct
 * relation, each containing one or more attribute numbers that are of
 * the domain type.  We have opened each rel and acquired the specified lock
 * type on it.
 *
 * We support nested domains by including attributes that are of derived
 * domain types.  Current callers do not need to distinguish between attributes
 * that are of exactly the given domain and those that are of derived domains.
 *
 * XXX this is completely broken because there is no way to lock the domain
 * to prevent columns from being added or dropped while our command runs.
 * We can partially protect against column drops by locking relations as we
 * come across them, but there is still a race condition (the window between
 * seeing a pg_depend entry and acquiring lock on the relation it references).
 * Also, holding locks on all these relations simultaneously creates a non-
 * trivial risk of deadlock.  We can minimize but not eliminate the deadlock
 * risk by using the weakest suitable lock (ShareLock for most callers).
 *
 * XXX the API for this is not sufficient to support checking domain values
 * that are inside container types, such as composite types, arrays, or
 * ranges.  Currently we just error out if a container type containing the
 * target domain is stored anywhere.
 *
 * Generally used for retrieving a list of tests when adding
 * new constraints to a domain.
 */
static List *
get_rels_with_domain(Oid domainOid, LOCKMODE lockmode)
{
	List	   *result = NIL;
	char	   *domainTypeName = format_type_be(domainOid);
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	Assert(lockmode != NoLock);

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	/*
	 * We scan pg_depend to find those things that depend on the domain. (We
	 * assume we can ignore refobjsubid for a domain.)
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		RelToCheck *rtc = NULL;
		ListCell   *rellist;
		Form_pg_attribute pg_att;
		int			ptr;

		/* Check for directly dependent types */
		if (pg_depend->classid == TypeRelationId)
		{
			if (get_typtype(pg_depend->objid) == TYPTYPE_DOMAIN)
			{
				/*
				 * This is a sub-domain, so recursively add dependent columns
				 * to the output list.  This is a bit inefficient since we may
				 * fail to combine RelToCheck entries when attributes of the
				 * same rel have different derived domain types, but it's
				 * probably not worth improving.
				 */
				result = list_concat(result,
									 get_rels_with_domain(pg_depend->objid,
														  lockmode));
			}
			else
			{
				/*
				 * Otherwise, it is some container type using the domain, so
				 * fail if there are any columns of this type.
				 */
				find_composite_type_dependencies(pg_depend->objid,
												 NULL,
												 domainTypeName);
			}
			continue;
		}

		/* Else, ignore dependees that aren't user columns of relations */
		/* (we assume system columns are never of domain types) */
		if (pg_depend->classid != RelationRelationId ||
			pg_depend->objsubid <= 0)
			continue;

		/* See if we already have an entry for this relation */
		foreach(rellist, result)
		{
			RelToCheck *rt = (RelToCheck *) lfirst(rellist);

			if (RelationGetRelid(rt->rel) == pg_depend->objid)
			{
				rtc = rt;
				break;
			}
		}

		if (rtc == NULL)
		{
			/* First attribute found for this relation */
			Relation	rel;

			/* Acquire requested lock on relation */
			rel = relation_open(pg_depend->objid, lockmode);

			/*
			 * Check to see if rowtype is stored anyplace as a composite-type
			 * column; if so we have to fail, for now anyway.
			 */
			if (OidIsValid(rel->rd_rel->reltype))
				find_composite_type_dependencies(rel->rd_rel->reltype,
												 NULL,
												 domainTypeName);

			/*
			 * Otherwise, we can ignore relations except those with both
			 * storage and user-chosen column types.
			 *
			 * XXX If an index-only scan could satisfy "col::some_domain" from
			 * a suitable expression index, this should also check expression
			 * index columns.
			 */
			if (rel->rd_rel->relkind != RELKIND_RELATION &&
				rel->rd_rel->relkind != RELKIND_MATVIEW)
			{
				relation_close(rel, lockmode);
				continue;
			}

			/* Build the RelToCheck entry with enough space for all atts */
			rtc = (RelToCheck *) palloc(sizeof(RelToCheck));
			rtc->rel = rel;
			rtc->natts = 0;
			rtc->atts = (int *) palloc(sizeof(int) * RelationGetNumberOfAttributes(rel));
			result = lappend(result, rtc);
		}

		/*
		 * Confirm column has not been dropped, and is of the expected type.
		 * This defends against an ALTER DROP COLUMN occurring just before we
		 * acquired lock ... but if the whole table were dropped, we'd still
		 * have a problem.
		 */
		if (pg_depend->objsubid > RelationGetNumberOfAttributes(rtc->rel))
			continue;
		pg_att = TupleDescAttr(rtc->rel->rd_att, pg_depend->objsubid - 1);
		if (pg_att->attisdropped || pg_att->atttypid != domainOid)
			continue;

		/*
		 * Okay, add column to result.  We store the columns in column-number
		 * order; this is just a hack to improve predictability of regression
		 * test output ...
		 */
		Assert(rtc->natts < RelationGetNumberOfAttributes(rtc->rel));

		ptr = rtc->natts++;
		while (ptr > 0 && rtc->atts[ptr - 1] > pg_depend->objsubid)
		{
			rtc->atts[ptr] = rtc->atts[ptr - 1];
			ptr--;
		}
		rtc->atts[ptr] = pg_depend->objsubid;
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	return result;
}

/*
 * checkDomainOwner
 *
 * Check that the type is actually a domain and that the current user
 * has permission to do ALTER DOMAIN on it.  Throw an error if not.
 */
void
checkDomainOwner(HeapTuple tup)
{
	Form_pg_type typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check that this is actually a domain */
	if (typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typTup->oid))));

	/* Permission check: must own type */
	if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);
}

/*
 * domainAddCheckConstraint - code shared between CREATE and ALTER DOMAIN
 */
static char *
domainAddCheckConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
						 int typMod, Constraint *constr,
						 const char *domainName, ObjectAddress *constrAddr)
{
	Node	   *expr;
	char	   *ccbin;
	ParseState *pstate;
	CoerceToDomainValue *domVal;
	Oid			ccoid;

	Assert(constr->contype == CONSTR_CHECK);

	/*
	 * Assign or validate constraint name
	 */
	if (constr->conname)
	{
		if (ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
								 domainOid,
								 constr->conname))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for domain \"%s\" already exists",
							constr->conname, domainName)));
	}
	else
		constr->conname = ChooseConstraintName(domainName,
											   NULL,
											   "check",
											   domainNamespace,
											   NIL);

	/*
	 * Convert the A_EXPR in raw_expr into an EXPR
	 */
	pstate = make_parsestate(NULL);

	/*
	 * Set up a CoerceToDomainValue to represent the occurrence of VALUE in
	 * the expression.  Note that it will appear to have the type of the base
	 * type, not the domain.  This seems correct since within the check
	 * expression, we should not assume the input value can be considered a
	 * member of the domain.
	 */
	domVal = makeNode(CoerceToDomainValue);
	domVal->typeId = baseTypeOid;
	domVal->typeMod = typMod;
	domVal->collation = get_typcollation(baseTypeOid);
	domVal->location = -1;		/* will be set when/if used */

	pstate->p_pre_columnref_hook = replace_domain_constraint_value;
	pstate->p_ref_hook_state = domVal;

	expr = transformExpr(pstate, constr->raw_expr, EXPR_KIND_DOMAIN_CHECK);

	/*
	 * Make sure it yields a boolean result.
	 */
	expr = coerce_to_boolean(pstate, expr, "CHECK");

	/*
	 * Fix up collation information.
	 */
	assign_expr_collations(pstate, expr);

	/*
	 * Domains don't allow variables (this is probably dead code now that
	 * add_missing_from is history, but let's be sure).
	 */
	if (pstate->p_rtable != NIL ||
		contain_var_clause(expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use table references in domain check constraint")));

	/*
	 * Convert to string form for storage.
	 */
	ccbin = nodeToString(expr);

	/*
	 * Store the constraint in pg_constraint
	 */
	ccoid =
		CreateConstraintEntry(constr->conname,	/* Constraint Name */
							  domainNamespace,	/* namespace */
							  CONSTRAINT_CHECK, /* Constraint Type */
							  false,	/* Is Deferrable */
							  false,	/* Is Deferred */
							  !constr->skip_validation, /* Is Validated */
							  InvalidOid,	/* no parent constraint */
							  InvalidOid,	/* not a relation constraint */
							  NULL,
							  0,
							  0,
							  domainOid,	/* domain constraint */
							  InvalidOid,	/* no associated index */
							  InvalidOid,	/* Foreign key fields */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* not an exclusion constraint */
							  expr, /* Tree form of check constraint */
							  ccbin,	/* Binary form of check constraint */
							  true, /* is local */
							  0,	/* inhcount */
							  false,	/* connoinherit */
							  false,	/* conperiod */
							  false);	/* is_internal */
	if (constrAddr)
		ObjectAddressSet(*constrAddr, ConstraintRelationId, ccoid);

	/*
	 * Return the compiled constraint expression so the calling routine can
	 * perform any additional required tests.
	 */
	return ccbin;
}

/* Parser pre_columnref_hook for domain CHECK constraint parsing */
static Node *
replace_domain_constraint_value(ParseState *pstate, ColumnRef *cref)
{
	/*
	 * Check for a reference to "value", and if that's what it is, replace
	 * with a CoerceToDomainValue as prepared for us by
	 * domainAddCheckConstraint. (We handle VALUE as a name, not a keyword, to
	 * avoid breaking a lot of applications that have used VALUE as a column
	 * name in the past.)
	 */
	if (list_length(cref->fields) == 1)
	{
		Node	   *field1 = (Node *) linitial(cref->fields);
		char	   *colname;

		colname = strVal(field1);
		if (strcmp(colname, "value") == 0)
		{
			CoerceToDomainValue *domVal = copyObject(pstate->p_ref_hook_state);

			/* Propagate location knowledge, if any */
			domVal->location = cref->location;
			return (Node *) domVal;
		}
	}
	return NULL;
}

/*
 * domainAddNotNullConstraint - code shared between CREATE and ALTER DOMAIN
 */
static void
domainAddNotNullConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
						   int typMod, Constraint *constr,
						   const char *domainName, ObjectAddress *constrAddr)
{
	Oid			ccoid;

	Assert(constr->contype == CONSTR_NOTNULL);

	/*
	 * Assign or validate constraint name
	 */
	if (constr->conname)
	{
		if (ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
								 domainOid,
								 constr->conname))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for domain \"%s\" already exists",
							constr->conname, domainName)));
	}
	else
		constr->conname = ChooseConstraintName(domainName,
											   NULL,
											   "not_null",
											   domainNamespace,
											   NIL);

	/*
	 * Store the constraint in pg_constraint
	 */
	ccoid =
		CreateConstraintEntry(constr->conname,	/* Constraint Name */
							  domainNamespace,	/* namespace */
							  CONSTRAINT_NOTNULL,	/* Constraint Type */
							  false,	/* Is Deferrable */
							  false,	/* Is Deferred */
							  !constr->skip_validation, /* Is Validated */
							  InvalidOid,	/* no parent constraint */
							  InvalidOid,	/* not a relation constraint */
							  NULL,
							  0,
							  0,
							  domainOid,	/* domain constraint */
							  InvalidOid,	/* no associated index */
							  InvalidOid,	/* Foreign key fields */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* not an exclusion constraint */
							  NULL,
							  NULL,
							  true, /* is local */
							  0,	/* inhcount */
							  false,	/* connoinherit */
							  false,	/* conperiod */
							  false);	/* is_internal */

	if (constrAddr)
		ObjectAddressSet(*constrAddr, ConstraintRelationId, ccoid);
}


/*
 * Execute ALTER TYPE RENAME
 */
ObjectAddress
RenameType(RenameStmt *stmt)
{
	List	   *names = castNode(List, stmt->object);
	const char *newTypeName = stmt->newname;
	TypeName   *typename;
	Oid			typeOid;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* Look up the type in the type table */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* check permissions on type */
	if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/* ALTER DOMAIN used on a non-domain? */
	if (stmt->renameType == OBJECT_DOMAIN && typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/*
	 * If it's a composite type, we need to check that it really is a
	 * free-standing composite type, and not a table's rowtype. We want people
	 * to use ALTER TABLE not ALTER TYPE for that case.
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE &&
		get_rel_relkind(typTup->typrelid) != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	/* don't allow direct alteration of array types, either */
	if (IsTrueArrayType(typTup))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(typTup->typelem))));

	/* we do allow separate renaming of multirange types, though */

	/*
	 * If type is composite we need to rename associated pg_class entry too.
	 * RenameRelationInternal will call RenameTypeInternal automatically.
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE)
		RenameRelationInternal(typTup->typrelid, newTypeName, false, false);
	else
		RenameTypeInternal(typeOid, newTypeName,
						   typTup->typnamespace);

	ObjectAddressSet(address, TypeRelationId, typeOid);
	/* Clean up */
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change the owner of a type.
 */
ObjectAddress
AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype)
{
	TypeName   *typename;
	Oid			typeOid;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	newtup;
	Form_pg_type typTup;
	AclResult	aclresult;
	ObjectAddress address;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);

	/* Use LookupTypeName here so that shell types can be processed */
	tup = LookupTypeName(NULL, typename, NULL, false);
	if (tup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typename))));
	typeOid = typeTypeId(tup);

	/* Copy the syscache entry so we can scribble on it below */
	newtup = heap_copytuple(tup);
	ReleaseSysCache(tup);
	tup = newtup;
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Don't allow ALTER DOMAIN on a type */
	if (objecttype == OBJECT_DOMAIN && typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/*
	 * If it's a composite type, we need to check that it really is a
	 * free-standing composite type, and not a table's rowtype. We want people
	 * to use ALTER TABLE not ALTER TYPE for that case.
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE &&
		get_rel_relkind(typTup->typrelid) != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	/* don't allow direct alteration of array types, either */
	if (IsTrueArrayType(typTup))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(typTup->typelem))));

	/* don't allow direct alteration of multirange types, either */
	if (typTup->typtype == TYPTYPE_MULTIRANGE)
	{
		Oid			rangetype = get_multirange_range(typeOid);

		/* We don't expect get_multirange_range to fail, but cope if so */
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter multirange type %s",
						format_type_be(typeOid)),
				 OidIsValid(rangetype) ?
				 errhint("You can alter type %s, which will alter the multirange type as well.",
						 format_type_be(rangetype)) : 0));
	}

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (typTup->typowner != newOwnerId)
	{
		/* Superusers can always do it */
		if (!superuser())
		{
			/* Otherwise, must be owner of the existing object */
			if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
				aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);

			/* Must be able to become new owner */
			check_can_set_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			aclresult = object_aclcheck(NamespaceRelationId, typTup->typnamespace,
										newOwnerId,
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_SCHEMA,
							   get_namespace_name(typTup->typnamespace));
		}

		AlterTypeOwner_oid(typeOid, newOwnerId, true);
	}

	ObjectAddressSet(address, TypeRelationId, typeOid);

	/* Clean up */
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * AlterTypeOwner_oid - change type owner unconditionally
 *
 * This function recurses to handle dependent types (arrays and multiranges).
 * It invokes any necessary access object hooks.  If hasDependEntry is true,
 * this function modifies the pg_shdepend entry appropriately (this should be
 * passed as false only for table rowtypes and dependent types).
 *
 * This is used by ALTER TABLE/TYPE OWNER commands, as well as by REASSIGN
 * OWNED BY.  It assumes the caller has done all needed checks.
 */
void
AlterTypeOwner_oid(Oid typeOid, Oid newOwnerId, bool hasDependEntry)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/*
	 * If it's a composite type, invoke ATExecChangeOwner so that we fix up
	 * the pg_class entry properly.  That will call back to
	 * AlterTypeOwnerInternal to take care of the pg_type entry(s).
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE)
		ATExecChangeOwner(typTup->typrelid, newOwnerId, true, AccessExclusiveLock);
	else
		AlterTypeOwnerInternal(typeOid, newOwnerId);

	/* Update owner dependency reference */
	if (hasDependEntry)
		changeDependencyOnOwner(TypeRelationId, typeOid, newOwnerId);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	ReleaseSysCache(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * AlterTypeOwnerInternal - bare-bones type owner change.
 *
 * This routine simply modifies the owner of a pg_type entry, and recurses
 * to handle any dependent types.
 */
void
AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;
	Datum		repl_val[Natts_pg_type];
	bool		repl_null[Natts_pg_type];
	bool		repl_repl[Natts_pg_type];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	repl_repl[Anum_pg_type_typowner - 1] = true;
	repl_val[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(newOwnerId);

	aclDatum = heap_getattr(tup,
							Anum_pg_type_typacl,
							RelationGetDescr(rel),
							&isNull);
	/* Null ACLs do not require changes */
	if (!isNull)
	{
		newAcl = aclnewowner(DatumGetAclP(aclDatum),
							 typTup->typowner, newOwnerId);
		repl_repl[Anum_pg_type_typacl - 1] = true;
		repl_val[Anum_pg_type_typacl - 1] = PointerGetDatum(newAcl);
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
							repl_repl);

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* If it has an array type, update that too */
	if (OidIsValid(typTup->typarray))
		AlterTypeOwnerInternal(typTup->typarray, newOwnerId);

	/* If it is a range type, update the associated multirange too */
	if (typTup->typtype == TYPTYPE_RANGE)
	{
		Oid			multirange_typeid = get_range_multirange(typeOid);

		if (!OidIsValid(multirange_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find multirange type for data type %s",
							format_type_be(typeOid))));
		AlterTypeOwnerInternal(multirange_typeid, newOwnerId);
	}

	/* Clean up */
	table_close(rel, RowExclusiveLock);
}

/*
 * Execute ALTER TYPE SET SCHEMA
 */
ObjectAddress
AlterTypeNamespace(List *names, const char *newschema, ObjectType objecttype,
				   Oid *oldschema)
{
	TypeName   *typename;
	Oid			typeOid;
	Oid			nspOid;
	Oid			oldNspOid;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* Don't allow ALTER DOMAIN on a non-domain type */
	if (objecttype == OBJECT_DOMAIN && get_typtype(typeOid) != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/* get schema OID and check its permissions */
	nspOid = LookupCreationNamespace(newschema);

	objsMoved = new_object_addresses();
	oldNspOid = AlterTypeNamespace_oid(typeOid, nspOid, false, objsMoved);
	free_object_addresses(objsMoved);

	if (oldschema)
		*oldschema = oldNspOid;

	ObjectAddressSet(myself, TypeRelationId, typeOid);

	return myself;
}

/*
 * ALTER TYPE SET SCHEMA, where the caller has already looked up the OIDs
 * of the type and the target schema and checked the schema's privileges.
 *
 * If ignoreDependent is true, we silently ignore dependent types
 * (array types and table rowtypes) rather than raising errors.
 *
 * This entry point is exported for use by AlterObjectNamespace_oid,
 * which doesn't want errors when it passes OIDs of dependent types.
 *
 * Returns the type's old namespace OID, or InvalidOid if we did nothing.
 */
Oid
AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, bool ignoreDependent,
					   ObjectAddresses *objsMoved)
{
	Oid			elemOid;

	/* check permissions on type */
	if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/* don't allow direct alteration of array types */
	elemOid = get_element_type(typeOid);
	if (OidIsValid(elemOid) && get_array_type(elemOid) == typeOid)
	{
		if (ignoreDependent)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(elemOid))));
	}

	/* and do the work */
	return AlterTypeNamespaceInternal(typeOid, nspOid,
									  false,	/* isImplicitArray */
									  ignoreDependent,	/* ignoreDependent */
									  true, /* errorOnTableType */
									  objsMoved);
}

/*
 * Move specified type to new namespace.
 *
 * Caller must have already checked privileges.
 *
 * The function automatically recurses to process the type's array type,
 * if any.  isImplicitArray should be true only when doing this internal
 * recursion (outside callers must never try to move an array type directly).
 *
 * If ignoreDependent is true, we silently don't process table types.
 *
 * If errorOnTableType is true, the function errors out if the type is
 * a table type.  ALTER TABLE has to be used to move a table to a new
 * namespace.  (This flag is ignored if ignoreDependent is true.)
 *
 * We also do nothing if the type is already listed in *objsMoved.
 * After a successful move, we add the type to *objsMoved.
 *
 * Returns the type's old namespace OID, or InvalidOid if we did nothing.
 */
Oid
AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool ignoreDependent,
						   bool errorOnTableType,
						   ObjectAddresses *objsMoved)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typform;
	Oid			oldNspOid;
	Oid			arrayOid;
	bool		isCompositeType;
	ObjectAddress thisobj;

	/*
	 * Make sure we haven't moved this object previously.
	 */
	thisobj.classId = TypeRelationId;
	thisobj.objectId = typeOid;
	thisobj.objectSubId = 0;

	if (object_address_present(&thisobj, objsMoved))
		return InvalidOid;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typform = (Form_pg_type) GETSTRUCT(tup);

	oldNspOid = typform->typnamespace;
	arrayOid = typform->typarray;

	/* If the type is already there, we scan skip these next few checks. */
	if (oldNspOid != nspOid)
	{
		/* common checks on switching namespaces */
		CheckSetNamespace(oldNspOid, nspOid);

		/* check for duplicate name (more friendly than unique-index failure) */
		if (SearchSysCacheExists2(TYPENAMENSP,
								  NameGetDatum(&typform->typname),
								  ObjectIdGetDatum(nspOid)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists in schema \"%s\"",
							NameStr(typform->typname),
							get_namespace_name(nspOid))));
	}

	/* Detect whether type is a composite type (but not a table rowtype) */
	isCompositeType =
		(typform->typtype == TYPTYPE_COMPOSITE &&
		 get_rel_relkind(typform->typrelid) == RELKIND_COMPOSITE_TYPE);

	/* Enforce not-table-type if requested */
	if (typform->typtype == TYPTYPE_COMPOSITE && !isCompositeType)
	{
		if (ignoreDependent)
		{
			table_close(rel, RowExclusiveLock);
			return InvalidOid;
		}
		if (errorOnTableType)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s is a table's row type",
							format_type_be(typeOid)),
			/* translator: %s is an SQL ALTER command */
					 errhint("Use %s instead.", "ALTER TABLE")));
	}

	if (oldNspOid != nspOid)
	{
		/* OK, modify the pg_type row */

		/* tup is a copy, so we can scribble directly on it */
		typform->typnamespace = nspOid;

		CatalogTupleUpdate(rel, &tup->t_self, tup);
	}

	/*
	 * Composite types have pg_class entries.
	 *
	 * We need to modify the pg_class tuple as well to reflect the change of
	 * schema.
	 */
	if (isCompositeType)
	{
		Relation	classRel;

		classRel = table_open(RelationRelationId, RowExclusiveLock);

		AlterRelationNamespaceInternal(classRel, typform->typrelid,
									   oldNspOid, nspOid,
									   false, objsMoved);

		table_close(classRel, RowExclusiveLock);

		/*
		 * Check for constraints associated with the composite type (we don't
		 * currently support this, but probably will someday).
		 */
		AlterConstraintNamespaces(typform->typrelid, oldNspOid,
								  nspOid, false, objsMoved);
	}
	else
	{
		/* If it's a domain, it might have constraints */
		if (typform->typtype == TYPTYPE_DOMAIN)
			AlterConstraintNamespaces(typeOid, oldNspOid, nspOid, true,
									  objsMoved);
	}

	/*
	 * Update dependency on schema, if any --- a table rowtype has not got
	 * one, and neither does an implicit array.
	 */
	if (oldNspOid != nspOid &&
		(isCompositeType || typform->typtype != TYPTYPE_COMPOSITE) &&
		!isImplicitArray)
		if (changeDependencyFor(TypeRelationId, typeOid,
								NamespaceRelationId, oldNspOid, nspOid) != 1)
			elog(ERROR, "could not change schema dependency for type \"%s\"",
				 format_type_be(typeOid));

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	add_exact_object_address(&thisobj, objsMoved);

	/* Recursively alter the associated array type, if any */
	if (OidIsValid(arrayOid))
		AlterTypeNamespaceInternal(arrayOid, nspOid,
								   true,	/* isImplicitArray */
								   false,	/* ignoreDependent */
								   true,	/* errorOnTableType */
								   objsMoved);

	return oldNspOid;
}

/*
 * AlterType
 *		ALTER TYPE <type> SET (option = ...)
 *
 * NOTE: the set of changes that can be allowed here is constrained by many
 * non-obvious implementation restrictions.  Tread carefully when considering
 * adding new flexibility.
 */
ObjectAddress
AlterType(AlterTypeStmt *stmt)
{
	ObjectAddress address;
	Relation	catalog;
	TypeName   *typename;
	HeapTuple	tup;
	Oid			typeOid;
	Form_pg_type typForm;
	bool		requireSuper = false;
	AlterTypeRecurseParams atparams;
	ListCell   *pl;

	catalog = table_open(TypeRelationId, RowExclusiveLock);

	/* Make a TypeName so we can use standard type lookup machinery */
	typename = makeTypeNameFromNameList(stmt->typeName);
	tup = typenameType(NULL, typename, NULL);

	typeOid = typeTypeId(tup);
	typForm = (Form_pg_type) GETSTRUCT(tup);

	/* Process options */
	memset(&atparams, 0, sizeof(atparams));
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "storage") == 0)
		{
			char	   *a = defGetString(defel);

			if (pg_strcasecmp(a, "plain") == 0)
				atparams.storage = TYPSTORAGE_PLAIN;
			else if (pg_strcasecmp(a, "external") == 0)
				atparams.storage = TYPSTORAGE_EXTERNAL;
			else if (pg_strcasecmp(a, "extended") == 0)
				atparams.storage = TYPSTORAGE_EXTENDED;
			else if (pg_strcasecmp(a, "main") == 0)
				atparams.storage = TYPSTORAGE_MAIN;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("storage \"%s\" not recognized", a)));

			/*
			 * Validate the storage request.  If the type isn't varlena, it
			 * certainly doesn't support non-PLAIN storage.
			 */
			if (atparams.storage != TYPSTORAGE_PLAIN && typForm->typlen != -1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("fixed-size types must have storage PLAIN")));

			/*
			 * Switching from PLAIN to non-PLAIN is allowed, but it requires
			 * superuser, since we can't validate that the type's C functions
			 * will support it.  Switching from non-PLAIN to PLAIN is
			 * disallowed outright, because it's not practical to ensure that
			 * no tables have toasted values of the type.  Switching among
			 * different non-PLAIN settings is OK, since it just constitutes a
			 * change in the strategy requested for columns created in the
			 * future.
			 */
			if (atparams.storage != TYPSTORAGE_PLAIN &&
				typForm->typstorage == TYPSTORAGE_PLAIN)
				requireSuper = true;
			else if (atparams.storage == TYPSTORAGE_PLAIN &&
					 typForm->typstorage != TYPSTORAGE_PLAIN)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot change type's storage to PLAIN")));

			atparams.updateStorage = true;
		}
		else if (strcmp(defel->defname, "receive") == 0)
		{
			if (defel->arg != NULL)
				atparams.receiveOid =
					findTypeReceiveFunction(defGetQualifiedName(defel),
											typeOid);
			else
				atparams.receiveOid = InvalidOid;	/* NONE, remove function */
			atparams.updateReceive = true;
			/* Replacing an I/O function requires superuser. */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "send") == 0)
		{
			if (defel->arg != NULL)
				atparams.sendOid =
					findTypeSendFunction(defGetQualifiedName(defel),
										 typeOid);
			else
				atparams.sendOid = InvalidOid;	/* NONE, remove function */
			atparams.updateSend = true;
			/* Replacing an I/O function requires superuser. */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "typmod_in") == 0)
		{
			if (defel->arg != NULL)
				atparams.typmodinOid =
					findTypeTypmodinFunction(defGetQualifiedName(defel));
			else
				atparams.typmodinOid = InvalidOid;	/* NONE, remove function */
			atparams.updateTypmodin = true;
			/* Replacing an I/O function requires superuser. */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "typmod_out") == 0)
		{
			if (defel->arg != NULL)
				atparams.typmodoutOid =
					findTypeTypmodoutFunction(defGetQualifiedName(defel));
			else
				atparams.typmodoutOid = InvalidOid; /* NONE, remove function */
			atparams.updateTypmodout = true;
			/* Replacing an I/O function requires superuser. */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "analyze") == 0)
		{
			if (defel->arg != NULL)
				atparams.analyzeOid =
					findTypeAnalyzeFunction(defGetQualifiedName(defel),
											typeOid);
			else
				atparams.analyzeOid = InvalidOid;	/* NONE, remove function */
			atparams.updateAnalyze = true;
			/* Replacing an analyze function requires superuser. */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "subscript") == 0)
		{
			if (defel->arg != NULL)
				atparams.subscriptOid =
					findTypeSubscriptingFunction(defGetQualifiedName(defel),
												 typeOid);
			else
				atparams.subscriptOid = InvalidOid; /* NONE, remove function */
			atparams.updateSubscript = true;
			/* Replacing a subscript function requires superuser. */
			requireSuper = true;
		}

		/*
		 * The rest of the options that CREATE accepts cannot be changed.
		 * Check for them so that we can give a meaningful error message.
		 */
		else if (strcmp(defel->defname, "input") == 0 ||
				 strcmp(defel->defname, "output") == 0 ||
				 strcmp(defel->defname, "internallength") == 0 ||
				 strcmp(defel->defname, "passedbyvalue") == 0 ||
				 strcmp(defel->defname, "alignment") == 0 ||
				 strcmp(defel->defname, "like") == 0 ||
				 strcmp(defel->defname, "category") == 0 ||
				 strcmp(defel->defname, "preferred") == 0 ||
				 strcmp(defel->defname, "default") == 0 ||
				 strcmp(defel->defname, "element") == 0 ||
				 strcmp(defel->defname, "delimiter") == 0 ||
				 strcmp(defel->defname, "collatable") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" cannot be changed",
							defel->defname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * Permissions check.  Require superuser if we decided the command
	 * requires that, else must own the type.
	 */
	if (requireSuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter a type")));
	}
	else
	{
		if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
			aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);
	}

	/*
	 * We disallow all forms of ALTER TYPE SET on types that aren't plain base
	 * types.  It would for example be highly unsafe, not to mention
	 * pointless, to change the send/receive functions for a composite type.
	 * Moreover, pg_dump has no support for changing these properties on
	 * non-base types.  We might weaken this someday, but not now.
	 *
	 * Note: if you weaken this enough to allow composite types, be sure to
	 * adjust the GenerateTypeDependencies call in AlterTypeRecurse.
	 */
	if (typForm->typtype != TYPTYPE_BASE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a base type",
						format_type_be(typeOid))));

	/*
	 * For the same reasons, don't allow direct alteration of array types.
	 */
	if (IsTrueArrayType(typForm))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a base type",
						format_type_be(typeOid))));

	/* OK, recursively update this type and any arrays/domains over it */
	AlterTypeRecurse(typeOid, false, tup, catalog, &atparams);

	/* Clean up */
	ReleaseSysCache(tup);

	table_close(catalog, RowExclusiveLock);

	ObjectAddressSet(address, TypeRelationId, typeOid);

	return address;
}

/*
 * AlterTypeRecurse: one recursion step for AlterType()
 *
 * Apply the changes specified by "atparams" to the type identified by
 * "typeOid", whose existing pg_type tuple is "tup".  If necessary,
 * recursively update its array type as well.  Then search for any domains
 * over this type, and recursively apply (most of) the same changes to those
 * domains.
 *
 * We need this because the system generally assumes that a domain inherits
 * many properties from its base type.  See DefineDomain() above for details
 * of what is inherited.  Arrays inherit a smaller number of properties,
 * but not none.
 *
 * There's a race condition here, in that some other transaction could
 * concurrently add another domain atop this base type; we'd miss updating
 * that one.  Hence, be wary of allowing ALTER TYPE to change properties for
 * which it'd be really fatal for a domain to be out of sync with its base
 * type (typlen, for example).  In practice, races seem unlikely to be an
 * issue for plausible use-cases for ALTER TYPE.  If one does happen, it could
 * be fixed by re-doing the same ALTER TYPE once all prior transactions have
 * committed.
 */
static void
AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
				 HeapTuple tup, Relation catalog,
				 AlterTypeRecurseParams *atparams)
{
	Datum		values[Natts_pg_type];
	bool		nulls[Natts_pg_type];
	bool		replaces[Natts_pg_type];
	HeapTuple	newtup;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	domainTup;

	/* Since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	/* Update the current type's tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(replaces, 0, sizeof(replaces));

	if (atparams->updateStorage)
	{
		replaces[Anum_pg_type_typstorage - 1] = true;
		values[Anum_pg_type_typstorage - 1] = CharGetDatum(atparams->storage);
	}
	if (atparams->updateReceive)
	{
		replaces[Anum_pg_type_typreceive - 1] = true;
		values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(atparams->receiveOid);
	}
	if (atparams->updateSend)
	{
		replaces[Anum_pg_type_typsend - 1] = true;
		values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(atparams->sendOid);
	}
	if (atparams->updateTypmodin)
	{
		replaces[Anum_pg_type_typmodin - 1] = true;
		values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(atparams->typmodinOid);
	}
	if (atparams->updateTypmodout)
	{
		replaces[Anum_pg_type_typmodout - 1] = true;
		values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(atparams->typmodoutOid);
	}
	if (atparams->updateAnalyze)
	{
		replaces[Anum_pg_type_typanalyze - 1] = true;
		values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(atparams->analyzeOid);
	}
	if (atparams->updateSubscript)
	{
		replaces[Anum_pg_type_typsubscript - 1] = true;
		values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(atparams->subscriptOid);
	}

	newtup = heap_modify_tuple(tup, RelationGetDescr(catalog),
							   values, nulls, replaces);

	CatalogTupleUpdate(catalog, &newtup->t_self, newtup);

	/* Rebuild dependencies for this type */
	GenerateTypeDependencies(newtup,
							 catalog,
							 NULL,	/* don't have defaultExpr handy */
							 NULL,	/* don't have typacl handy */
							 0, /* we rejected composite types above */
							 isImplicitArray,	/* it might be an array */
							 isImplicitArray,	/* dependent iff it's array */
							 false, /* don't touch extension membership */
							 true);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	/*
	 * Arrays inherit their base type's typmodin and typmodout, but none of
	 * the other properties we're concerned with here.  Recurse to the array
	 * type if needed.
	 */
	if (!isImplicitArray &&
		(atparams->updateTypmodin || atparams->updateTypmodout))
	{
		Oid			arrtypoid = ((Form_pg_type) GETSTRUCT(newtup))->typarray;

		if (OidIsValid(arrtypoid))
		{
			HeapTuple	arrtup;
			AlterTypeRecurseParams arrparams;

			arrtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(arrtypoid));
			if (!HeapTupleIsValid(arrtup))
				elog(ERROR, "cache lookup failed for type %u", arrtypoid);

			memset(&arrparams, 0, sizeof(arrparams));
			arrparams.updateTypmodin = atparams->updateTypmodin;
			arrparams.updateTypmodout = atparams->updateTypmodout;
			arrparams.typmodinOid = atparams->typmodinOid;
			arrparams.typmodoutOid = atparams->typmodoutOid;

			AlterTypeRecurse(arrtypoid, true, arrtup, catalog, &arrparams);

			ReleaseSysCache(arrtup);
		}
	}

	/*
	 * Now we need to recurse to domains.  However, some properties are not
	 * inherited by domains, so clear the update flags for those.
	 */
	atparams->updateReceive = false;	/* domains use F_DOMAIN_RECV */
	atparams->updateTypmodin = false;	/* domains don't have typmods */
	atparams->updateTypmodout = false;
	atparams->updateSubscript = false;	/* domains don't have subscriptors */

	/* Skip the scan if nothing remains to be done */
	if (!(atparams->updateStorage ||
		  atparams->updateSend ||
		  atparams->updateAnalyze))
		return;

	/* Search pg_type for possible domains over this type */
	ScanKeyInit(&key[0],
				Anum_pg_type_typbasetype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = systable_beginscan(catalog, InvalidOid, false,
							  NULL, 1, key);

	while ((domainTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_type domainForm = (Form_pg_type) GETSTRUCT(domainTup);

		/*
		 * Shouldn't have a nonzero typbasetype in a non-domain, but let's
		 * check
		 */
		if (domainForm->typtype != TYPTYPE_DOMAIN)
			continue;

		AlterTypeRecurse(domainForm->oid, false, domainTup, catalog, atparams);
	}

	systable_endscan(scan);
}
