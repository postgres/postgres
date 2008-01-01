/*-------------------------------------------------------------------------
 *
 * pg_operator.c
 *	  routines to support manipulation of the pg_operator relation
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/pg_operator.c,v 1.103 2008/01/01 19:45:48 momjian Exp $
 *
 * NOTES
 *	  these routines moved here from commands/define.c and somewhat cleaned up.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Oid OperatorGet(const char *operatorName,
			Oid operatorNamespace,
			Oid leftObjectId,
			Oid rightObjectId,
			bool *defined);

static Oid OperatorLookup(List *operatorName,
			   Oid leftObjectId,
			   Oid rightObjectId,
			   bool *defined);

static Oid OperatorShellMake(const char *operatorName,
				  Oid operatorNamespace,
				  Oid leftTypeId,
				  Oid rightTypeId);

static void OperatorUpd(Oid baseId, Oid commId, Oid negId);

static Oid get_other_operator(List *otherOp,
				   Oid otherLeftTypeId, Oid otherRightTypeId,
				   const char *operatorName, Oid operatorNamespace,
				   Oid leftTypeId, Oid rightTypeId,
				   bool isCommutator);

static void makeOperatorDependencies(HeapTuple tuple);


/*
 * Check whether a proposed operator name is legal
 *
 * This had better match the behavior of parser/scan.l!
 *
 * We need this because the parser is not smart enough to check that
 * the arguments of CREATE OPERATOR's COMMUTATOR, NEGATOR, etc clauses
 * are operator names rather than some other lexical entity.
 */
static bool
validOperatorName(const char *name)
{
	size_t		len = strlen(name);

	/* Can't be empty or too long */
	if (len == 0 || len >= NAMEDATALEN)
		return false;

	/* Can't contain any invalid characters */
	/* Test string here should match op_chars in scan.l */
	if (strspn(name, "~!@#^&|`?+-*/%<>=") != len)
		return false;

	/* Can't contain slash-star or dash-dash (comment starts) */
	if (strstr(name, "/*") || strstr(name, "--"))
		return false;

	/*
	 * For SQL92 compatibility, '+' and '-' cannot be the last char of a
	 * multi-char operator unless the operator contains chars that are not in
	 * SQL92 operators. The idea is to lex '=-' as two operators, but not to
	 * forbid operator names like '?-' that could not be sequences of SQL92
	 * operators.
	 */
	if (len > 1 &&
		(name[len - 1] == '+' ||
		 name[len - 1] == '-'))
	{
		int			ic;

		for (ic = len - 2; ic >= 0; ic--)
		{
			if (strchr("~!@#^&|`?%", name[ic]))
				break;
		}
		if (ic < 0)
			return false;		/* nope, not valid */
	}

	/* != isn't valid either, because parser will convert it to <> */
	if (strcmp(name, "!=") == 0)
		return false;

	return true;
}


/*
 * OperatorGet
 *
 *		finds an operator given an exact specification (name, namespace,
 *		left and right type IDs).
 *
 *		*defined is set TRUE if defined (not a shell)
 */
static Oid
OperatorGet(const char *operatorName,
			Oid operatorNamespace,
			Oid leftObjectId,
			Oid rightObjectId,
			bool *defined)
{
	HeapTuple	tup;
	Oid			operatorObjectId;

	tup = SearchSysCache(OPERNAMENSP,
						 PointerGetDatum(operatorName),
						 ObjectIdGetDatum(leftObjectId),
						 ObjectIdGetDatum(rightObjectId),
						 ObjectIdGetDatum(operatorNamespace));
	if (HeapTupleIsValid(tup))
	{
		RegProcedure oprcode = ((Form_pg_operator) GETSTRUCT(tup))->oprcode;

		operatorObjectId = HeapTupleGetOid(tup);
		*defined = RegProcedureIsValid(oprcode);
		ReleaseSysCache(tup);
	}
	else
	{
		operatorObjectId = InvalidOid;
		*defined = false;
	}

	return operatorObjectId;
}

/*
 * OperatorLookup
 *
 *		looks up an operator given a possibly-qualified name and
 *		left and right type IDs.
 *
 *		*defined is set TRUE if defined (not a shell)
 */
static Oid
OperatorLookup(List *operatorName,
			   Oid leftObjectId,
			   Oid rightObjectId,
			   bool *defined)
{
	Oid			operatorObjectId;
	RegProcedure oprcode;

	operatorObjectId = LookupOperName(NULL, operatorName,
									  leftObjectId, rightObjectId,
									  true, -1);
	if (!OidIsValid(operatorObjectId))
	{
		*defined = false;
		return InvalidOid;
	}

	oprcode = get_opcode(operatorObjectId);
	*defined = RegProcedureIsValid(oprcode);

	return operatorObjectId;
}


/*
 * OperatorShellMake
 *		Make a "shell" entry for a not-yet-existing operator.
 */
static Oid
OperatorShellMake(const char *operatorName,
				  Oid operatorNamespace,
				  Oid leftTypeId,
				  Oid rightTypeId)
{
	Relation	pg_operator_desc;
	Oid			operatorObjectId;
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_operator];
	char		nulls[Natts_pg_operator];
	NameData	oname;
	TupleDesc	tupDesc;

	/*
	 * validate operator name
	 */
	if (!validOperatorName(operatorName))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("\"%s\" is not a valid operator name",
						operatorName)));

	/*
	 * initialize our *nulls and *values arrays
	 */
	for (i = 0; i < Natts_pg_operator; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;		/* redundant, but safe */
	}

	/*
	 * initialize values[] with the operator name and input data types. Note
	 * that oprcode is set to InvalidOid, indicating it's a shell.
	 */
	i = 0;
	namestrcpy(&oname, operatorName);
	values[i++] = NameGetDatum(&oname); /* oprname */
	values[i++] = ObjectIdGetDatum(operatorNamespace);	/* oprnamespace */
	values[i++] = ObjectIdGetDatum(GetUserId());		/* oprowner */
	values[i++] = CharGetDatum(leftTypeId ? (rightTypeId ? 'b' : 'r') : 'l');	/* oprkind */
	values[i++] = BoolGetDatum(false);	/* oprcanmerge */
	values[i++] = BoolGetDatum(false);	/* oprcanhash */
	values[i++] = ObjectIdGetDatum(leftTypeId); /* oprleft */
	values[i++] = ObjectIdGetDatum(rightTypeId);		/* oprright */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* oprresult */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* oprcom */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* oprnegate */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* oprcode */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* oprrest */
	values[i++] = ObjectIdGetDatum(InvalidOid); /* oprjoin */

	/*
	 * open pg_operator
	 */
	pg_operator_desc = heap_open(OperatorRelationId, RowExclusiveLock);
	tupDesc = pg_operator_desc->rd_att;

	/*
	 * create a new operator tuple
	 */
	tup = heap_formtuple(tupDesc, values, nulls);

	/*
	 * insert our "shell" operator tuple
	 */
	operatorObjectId = simple_heap_insert(pg_operator_desc, tup);

	CatalogUpdateIndexes(pg_operator_desc, tup);

	/* Add dependencies for the entry */
	makeOperatorDependencies(tup);

	heap_freetuple(tup);

	/*
	 * close the operator relation and return the oid.
	 */
	heap_close(pg_operator_desc, RowExclusiveLock);

	return operatorObjectId;
}

/*
 * OperatorCreate
 *
 * "X" indicates an optional argument (i.e. one that can be NULL or 0)
 *		operatorName			name for new operator
 *		operatorNamespace		namespace for new operator
 *		leftTypeId				X left type ID
 *		rightTypeId				X right type ID
 *		procedureName			procedure for operator
 *		commutatorName			X commutator operator
 *		negatorName				X negator operator
 *		restrictionName			X restriction sel. procedure
 *		joinName				X join sel. procedure
 *		canMerge				merge join can be used with this operator
 *		canHash					hash join can be used with this operator
 *
 * This routine gets complicated because it allows the user to
 * specify operators that do not exist.  For example, if operator
 * "op" is being defined, the negator operator "negop" and the
 * commutator "commop" can also be defined without specifying
 * any information other than their names.	Since in order to
 * add "op" to the PG_OPERATOR catalog, all the Oid's for these
 * operators must be placed in the fields of "op", a forward
 * declaration is done on the commutator and negator operators.
 * This is called creating a shell, and its main effect is to
 * create a tuple in the PG_OPERATOR catalog with minimal
 * information about the operator (just its name and types).
 * Forward declaration is used only for this purpose, it is
 * not available to the user as it is for type definition.
 *
 * Algorithm:
 *
 * check if operator already defined
 *	  if so, but oprcode is null, save the Oid -- we are filling in a shell
 *	  otherwise error
 * get the attribute types from relation descriptor for pg_operator
 * assign values to the fields of the operator:
 *	 operatorName
 *	 owner id (simply the user id of the caller)
 *	 operator "kind" either "b" for binary or "l" for left unary
 *	 canMerge boolean
 *	 canHash boolean
 *	 leftTypeObjectId -- type must already be defined
 *	 rightTypeObjectId -- this is optional, enter ObjectId=0 if none specified
 *	 resultType -- defer this, since it must be determined from
 *				   the pg_procedure catalog
 *	 commutatorObjectId -- if this is NULL, enter ObjectId=0
 *					  else if this already exists, enter its ObjectId
 *					  else if this does not yet exist, and is not
 *						the same as the main operatorName, then create
 *						a shell and enter the new ObjectId
 *					  else if this does not exist but IS the same
 *						name & types as the main operator, set the ObjectId=0.
 *						(We are creating a self-commutating operator.)
 *						The link will be fixed later by OperatorUpd.
 *	 negatorObjectId   -- same as for commutatorObjectId
 *	 operatorProcedure -- must access the pg_procedure catalog to get the
 *				   ObjectId of the procedure that actually does the operator
 *				   actions this is required.  Do a lookup to find out the
 *				   return type of the procedure
 *	 restrictionProcedure -- must access the pg_procedure catalog to get
 *				   the ObjectId but this is optional
 *	 joinProcedure -- same as restrictionProcedure
 * now either insert or replace the operator into the pg_operator catalog
 * if the operator shell is being filled in
 *	 access the catalog in order to get a valid buffer
 *	 create a tuple using ModifyHeapTuple
 *	 get the t_self from the modified tuple and call RelationReplaceHeapTuple
 * else if a new operator is being created
 *	 create a tuple using heap_formtuple
 *	 call simple_heap_insert
 */
void
OperatorCreate(const char *operatorName,
			   Oid operatorNamespace,
			   Oid leftTypeId,
			   Oid rightTypeId,
			   List *procedureName,
			   List *commutatorName,
			   List *negatorName,
			   List *restrictionName,
			   List *joinName,
			   bool canMerge,
			   bool canHash)
{
	Relation	pg_operator_desc;
	HeapTuple	tup;
	char		nulls[Natts_pg_operator];
	char		replaces[Natts_pg_operator];
	Datum		values[Natts_pg_operator];
	Oid			operatorObjectId;
	bool		operatorAlreadyDefined;
	Oid			procOid;
	Oid			operResultType;
	Oid			commutatorId,
				negatorId,
				restOid,
				joinOid;
	bool		selfCommutator = false;
	Oid			typeId[4];		/* only need up to 4 args here */
	int			nargs;
	NameData	oname;
	TupleDesc	tupDesc;
	int			i;

	/*
	 * Sanity checks
	 */
	if (!validOperatorName(operatorName))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("\"%s\" is not a valid operator name",
						operatorName)));

	if (!OidIsValid(leftTypeId) && !OidIsValid(rightTypeId))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
		   errmsg("at least one of leftarg or rightarg must be specified")));

	if (!(OidIsValid(leftTypeId) && OidIsValid(rightTypeId)))
	{
		/* If it's not a binary op, these things mustn't be set: */
		if (commutatorName)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can have commutators")));
		if (joinName)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("only binary operators can have join selectivity")));
		if (canMerge)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can merge join")));
		if (canHash)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can hash")));
	}

	operatorObjectId = OperatorGet(operatorName,
								   operatorNamespace,
								   leftTypeId,
								   rightTypeId,
								   &operatorAlreadyDefined);

	if (operatorAlreadyDefined)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("operator %s already exists",
						operatorName)));

	/*
	 * At this point, if operatorObjectId is not InvalidOid then we are
	 * filling in a previously-created shell.
	 */

	/*
	 * Look up registered procedures -- find the return type of procedureName
	 * to place in "result" field. Do this before shells are created so we
	 * don't have to worry about deleting them later.
	 */
	if (!OidIsValid(leftTypeId))
	{
		typeId[0] = rightTypeId;
		nargs = 1;
	}
	else if (!OidIsValid(rightTypeId))
	{
		typeId[0] = leftTypeId;
		nargs = 1;
	}
	else
	{
		typeId[0] = leftTypeId;
		typeId[1] = rightTypeId;
		nargs = 2;
	}
	procOid = LookupFuncName(procedureName, nargs, typeId, false);
	operResultType = get_func_rettype(procOid);

	/*
	 * find restriction estimator
	 */
	if (restrictionName)
	{
		typeId[0] = INTERNALOID;	/* Query */
		typeId[1] = OIDOID;		/* operator OID */
		typeId[2] = INTERNALOID;	/* args list */
		typeId[3] = INT4OID;	/* varRelid */

		restOid = LookupFuncName(restrictionName, 4, typeId, false);
	}
	else
		restOid = InvalidOid;

	/*
	 * find join estimator
	 */
	if (joinName)
	{
		typeId[0] = INTERNALOID;	/* Query */
		typeId[1] = OIDOID;		/* operator OID */
		typeId[2] = INTERNALOID;	/* args list */
		typeId[3] = INT2OID;	/* jointype */

		joinOid = LookupFuncName(joinName, 4, typeId, false);
	}
	else
		joinOid = InvalidOid;

	/*
	 * set up values in the operator tuple
	 */

	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) NULL;
		replaces[i] = 'r';
		nulls[i] = ' ';
	}

	i = 0;
	namestrcpy(&oname, operatorName);
	values[i++] = NameGetDatum(&oname); /* oprname */
	values[i++] = ObjectIdGetDatum(operatorNamespace);	/* oprnamespace */
	values[i++] = ObjectIdGetDatum(GetUserId());		/* oprowner */
	values[i++] = CharGetDatum(leftTypeId ? (rightTypeId ? 'b' : 'r') : 'l');	/* oprkind */
	values[i++] = BoolGetDatum(canMerge);		/* oprcanmerge */
	values[i++] = BoolGetDatum(canHash);		/* oprcanhash */
	values[i++] = ObjectIdGetDatum(leftTypeId); /* oprleft */
	values[i++] = ObjectIdGetDatum(rightTypeId);		/* oprright */
	values[i++] = ObjectIdGetDatum(operResultType);		/* oprresult */

	/*
	 * Set up the other operators.	If they do not currently exist, create
	 * shells in order to get ObjectId's.
	 */

	if (commutatorName)
	{
		/* commutator has reversed arg types */
		commutatorId = get_other_operator(commutatorName,
										  rightTypeId, leftTypeId,
										  operatorName, operatorNamespace,
										  leftTypeId, rightTypeId,
										  true);

		/*
		 * self-linkage to this operator; will fix below. Note that only
		 * self-linkage for commutation makes sense.
		 */
		if (!OidIsValid(commutatorId))
			selfCommutator = true;
	}
	else
		commutatorId = InvalidOid;
	values[i++] = ObjectIdGetDatum(commutatorId);		/* oprcom */

	if (negatorName)
	{
		/* negator has same arg types */
		negatorId = get_other_operator(negatorName,
									   leftTypeId, rightTypeId,
									   operatorName, operatorNamespace,
									   leftTypeId, rightTypeId,
									   false);
	}
	else
		negatorId = InvalidOid;
	values[i++] = ObjectIdGetDatum(negatorId);	/* oprnegate */

	values[i++] = ObjectIdGetDatum(procOid);	/* oprcode */
	values[i++] = ObjectIdGetDatum(restOid);	/* oprrest */
	values[i++] = ObjectIdGetDatum(joinOid);	/* oprjoin */

	pg_operator_desc = heap_open(OperatorRelationId, RowExclusiveLock);

	/*
	 * If we are adding to an operator shell, update; else insert
	 */
	if (operatorObjectId)
	{
		tup = SearchSysCacheCopy(OPEROID,
								 ObjectIdGetDatum(operatorObjectId),
								 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u",
				 operatorObjectId);

		tup = heap_modifytuple(tup,
							   RelationGetDescr(pg_operator_desc),
							   values,
							   nulls,
							   replaces);

		simple_heap_update(pg_operator_desc, &tup->t_self, tup);
	}
	else
	{
		tupDesc = pg_operator_desc->rd_att;
		tup = heap_formtuple(tupDesc, values, nulls);

		operatorObjectId = simple_heap_insert(pg_operator_desc, tup);
	}

	/* Must update the indexes in either case */
	CatalogUpdateIndexes(pg_operator_desc, tup);

	/* Add dependencies for the entry */
	makeOperatorDependencies(tup);

	heap_close(pg_operator_desc, RowExclusiveLock);

	/*
	 * If a commutator and/or negator link is provided, update the other
	 * operator(s) to point at this one, if they don't already have a link.
	 * This supports an alternative style of operator definition wherein the
	 * user first defines one operator without giving negator or commutator,
	 * then defines the other operator of the pair with the proper commutator
	 * or negator attribute.  That style doesn't require creation of a shell,
	 * and it's the only style that worked right before Postgres version 6.5.
	 * This code also takes care of the situation where the new operator is
	 * its own commutator.
	 */
	if (selfCommutator)
		commutatorId = operatorObjectId;

	if (OidIsValid(commutatorId) || OidIsValid(negatorId))
		OperatorUpd(operatorObjectId, commutatorId, negatorId);
}

/*
 * Try to lookup another operator (commutator, etc)
 *
 * If not found, check to see if it is exactly the operator we are trying
 * to define; if so, return InvalidOid.  (Note that this case is only
 * sensible for a commutator, so we error out otherwise.)  If it is not
 * the same operator, create a shell operator.
 */
static Oid
get_other_operator(List *otherOp, Oid otherLeftTypeId, Oid otherRightTypeId,
				   const char *operatorName, Oid operatorNamespace,
				   Oid leftTypeId, Oid rightTypeId, bool isCommutator)
{
	Oid			other_oid;
	bool		otherDefined;
	char	   *otherName;
	Oid			otherNamespace;
	AclResult	aclresult;

	other_oid = OperatorLookup(otherOp,
							   otherLeftTypeId,
							   otherRightTypeId,
							   &otherDefined);

	if (OidIsValid(other_oid))
	{
		/* other op already in catalogs */
		return other_oid;
	}

	otherNamespace = QualifiedNameGetCreationNamespace(otherOp,
													   &otherName);

	if (strcmp(otherName, operatorName) == 0 &&
		otherNamespace == operatorNamespace &&
		otherLeftTypeId == leftTypeId &&
		otherRightTypeId == rightTypeId)
	{
		/*
		 * self-linkage to this operator; caller will fix later. Note that
		 * only self-linkage for commutation makes sense.
		 */
		if (!isCommutator)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("operator cannot be its own negator or sort operator")));
		return InvalidOid;
	}

	/* not in catalogs, different from operator, so make shell */

	aclresult = pg_namespace_aclcheck(otherNamespace, GetUserId(),
									  ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(otherNamespace));

	other_oid = OperatorShellMake(otherName,
								  otherNamespace,
								  otherLeftTypeId,
								  otherRightTypeId);
	return other_oid;
}

/*
 * OperatorUpd
 *
 *	For a given operator, look up its negator and commutator operators.
 *	If they are defined, but their negator and commutator fields
 *	(respectively) are empty, then use the new operator for neg or comm.
 *	This solves a problem for users who need to insert two new operators
 *	which are the negator or commutator of each other.
 */
static void
OperatorUpd(Oid baseId, Oid commId, Oid negId)
{
	int			i;
	Relation	pg_operator_desc;
	HeapTuple	tup;
	char		nulls[Natts_pg_operator];
	char		replaces[Natts_pg_operator];
	Datum		values[Natts_pg_operator];

	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) 0;
		replaces[i] = ' ';
		nulls[i] = ' ';
	}

	/*
	 * check and update the commutator & negator, if necessary
	 *
	 * First make sure we can see them...
	 */
	CommandCounterIncrement();

	pg_operator_desc = heap_open(OperatorRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy(OPEROID,
							 ObjectIdGetDatum(commId),
							 0, 0, 0);

	/*
	 * if the commutator and negator are the same operator, do one update. XXX
	 * this is probably useless code --- I doubt it ever makes sense for
	 * commutator and negator to be the same thing...
	 */
	if (commId == negId)
	{
		if (HeapTupleIsValid(tup))
		{
			Form_pg_operator t = (Form_pg_operator) GETSTRUCT(tup);

			if (!OidIsValid(t->oprcom) || !OidIsValid(t->oprnegate))
			{
				if (!OidIsValid(t->oprnegate))
				{
					values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(baseId);
					replaces[Anum_pg_operator_oprnegate - 1] = 'r';
				}

				if (!OidIsValid(t->oprcom))
				{
					values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(baseId);
					replaces[Anum_pg_operator_oprcom - 1] = 'r';
				}

				tup = heap_modifytuple(tup,
									   RelationGetDescr(pg_operator_desc),
									   values,
									   nulls,
									   replaces);

				simple_heap_update(pg_operator_desc, &tup->t_self, tup);

				CatalogUpdateIndexes(pg_operator_desc, tup);
			}
		}

		heap_close(pg_operator_desc, RowExclusiveLock);

		return;
	}

	/* if commutator and negator are different, do two updates */

	if (HeapTupleIsValid(tup) &&
		!(OidIsValid(((Form_pg_operator) GETSTRUCT(tup))->oprcom)))
	{
		values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(baseId);
		replaces[Anum_pg_operator_oprcom - 1] = 'r';

		tup = heap_modifytuple(tup,
							   RelationGetDescr(pg_operator_desc),
							   values,
							   nulls,
							   replaces);

		simple_heap_update(pg_operator_desc, &tup->t_self, tup);

		CatalogUpdateIndexes(pg_operator_desc, tup);

		values[Anum_pg_operator_oprcom - 1] = (Datum) NULL;
		replaces[Anum_pg_operator_oprcom - 1] = ' ';
	}

	/* check and update the negator, if necessary */

	tup = SearchSysCacheCopy(OPEROID,
							 ObjectIdGetDatum(negId),
							 0, 0, 0);

	if (HeapTupleIsValid(tup) &&
		!(OidIsValid(((Form_pg_operator) GETSTRUCT(tup))->oprnegate)))
	{
		values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(baseId);
		replaces[Anum_pg_operator_oprnegate - 1] = 'r';

		tup = heap_modifytuple(tup,
							   RelationGetDescr(pg_operator_desc),
							   values,
							   nulls,
							   replaces);

		simple_heap_update(pg_operator_desc, &tup->t_self, tup);

		CatalogUpdateIndexes(pg_operator_desc, tup);
	}

	heap_close(pg_operator_desc, RowExclusiveLock);
}

/*
 * Create dependencies for a new operator (either a freshly inserted
 * complete operator, a new shell operator, or a just-updated shell).
 *
 * NB: the OidIsValid tests in this routine are necessary, in case
 * the given operator is a shell.
 */
static void
makeOperatorDependencies(HeapTuple tuple)
{
	Form_pg_operator oper = (Form_pg_operator) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;

	myself.classId = OperatorRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/* In case we are updating a shell, delete any existing entries */
	deleteDependencyRecordsFor(myself.classId, myself.objectId);
	deleteSharedDependencyRecordsFor(myself.classId, myself.objectId);

	/* Dependency on namespace */
	if (OidIsValid(oper->oprnamespace))
	{
		referenced.classId = NamespaceRelationId;
		referenced.objectId = oper->oprnamespace;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on left type */
	if (OidIsValid(oper->oprleft))
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = oper->oprleft;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on right type */
	if (OidIsValid(oper->oprright))
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = oper->oprright;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on result type */
	if (OidIsValid(oper->oprresult))
	{
		referenced.classId = TypeRelationId;
		referenced.objectId = oper->oprresult;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/*
	 * NOTE: we do not consider the operator to depend on the associated
	 * operators oprcom and oprnegate. We would not want to delete this
	 * operator if those go away, but only reset the link fields; which is not
	 * a function that the dependency code can presently handle.  (Something
	 * could perhaps be done with objectSubId though.)	For now, it's okay to
	 * let those links dangle if a referenced operator is removed.
	 */

	/* Dependency on implementation function */
	if (OidIsValid(oper->oprcode))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = oper->oprcode;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on restriction selectivity function */
	if (OidIsValid(oper->oprrest))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = oper->oprrest;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on join selectivity function */
	if (OidIsValid(oper->oprjoin))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = oper->oprjoin;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on owner */
	recordDependencyOnOwner(OperatorRelationId, HeapTupleGetOid(tuple),
							oper->oprowner);
}
