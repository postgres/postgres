/*-------------------------------------------------------------------------
 *
 * pg_operator.c
 *	  routines to support manipulation of the pg_operator relation
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_operator.c
 *
 * NOTES
 *	  these routines moved here from commands/define.c and somewhat cleaned up.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_operator_fn.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
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

static Oid get_other_operator(List *otherOp,
				   Oid otherLeftTypeId, Oid otherRightTypeId,
				   const char *operatorName, Oid operatorNamespace,
				   Oid leftTypeId, Oid rightTypeId,
				   bool isCommutator);


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
	 * For SQL standard compatibility, '+' and '-' cannot be the last char of
	 * a multi-char operator unless the operator contains chars that are not
	 * in SQL operators. The idea is to lex '=-' as two operators, but not to
	 * forbid operator names like '?-' that could not be sequences of standard
	 * SQL operators.
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

	tup = SearchSysCache4(OPERNAMENSP,
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
	bool		nulls[Natts_pg_operator];
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
		nulls[i] = false;
		values[i] = (Datum) NULL;		/* redundant, but safe */
	}

	/*
	 * initialize values[] with the operator name and input data types. Note
	 * that oprcode is set to InvalidOid, indicating it's a shell.
	 */
	namestrcpy(&oname, operatorName);
	values[Anum_pg_operator_oprname - 1] = NameGetDatum(&oname);
	values[Anum_pg_operator_oprnamespace - 1] = ObjectIdGetDatum(operatorNamespace);
	values[Anum_pg_operator_oprowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_operator_oprkind - 1] = CharGetDatum(leftTypeId ? (rightTypeId ? 'b' : 'r') : 'l');
	values[Anum_pg_operator_oprcanmerge - 1] = BoolGetDatum(false);
	values[Anum_pg_operator_oprcanhash - 1] = BoolGetDatum(false);
	values[Anum_pg_operator_oprleft - 1] = ObjectIdGetDatum(leftTypeId);
	values[Anum_pg_operator_oprright - 1] = ObjectIdGetDatum(rightTypeId);
	values[Anum_pg_operator_oprresult - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprcode - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprrest - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprjoin - 1] = ObjectIdGetDatum(InvalidOid);

	/*
	 * open pg_operator
	 */
	pg_operator_desc = heap_open(OperatorRelationId, RowExclusiveLock);
	tupDesc = pg_operator_desc->rd_att;

	/*
	 * create a new operator tuple
	 */
	tup = heap_form_tuple(tupDesc, values, nulls);

	/*
	 * insert our "shell" operator tuple
	 */
	operatorObjectId = simple_heap_insert(pg_operator_desc, tup);

	CatalogUpdateIndexes(pg_operator_desc, tup);

	/* Add dependencies for the entry */
	makeOperatorDependencies(tup, false);

	heap_freetuple(tup);

	/* Post creation hook for new shell operator */
	InvokeObjectPostCreateHook(OperatorRelationId, operatorObjectId, 0);

	/*
	 * Make sure the tuple is visible for subsequent lookups/updates.
	 */
	CommandCounterIncrement();

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
 *		procedureId				procedure ID for operator
 *		commutatorName			X commutator operator
 *		negatorName				X negator operator
 *		restrictionId			X restriction selectivity procedure ID
 *		joinId					X join selectivity procedure ID
 *		canMerge				merge join can be used with this operator
 *		canHash					hash join can be used with this operator
 *
 * The caller should have validated properties and permissions for the
 * objects passed as OID references.  We must handle the commutator and
 * negator operator references specially, however, since those need not
 * exist beforehand.
 *
 * This routine gets complicated because it allows the user to
 * specify operators that do not exist.  For example, if operator
 * "op" is being defined, the negator operator "negop" and the
 * commutator "commop" can also be defined without specifying
 * any information other than their names.  Since in order to
 * add "op" to the PG_OPERATOR catalog, all the Oid's for these
 * operators must be placed in the fields of "op", a forward
 * declaration is done on the commutator and negator operators.
 * This is called creating a shell, and its main effect is to
 * create a tuple in the PG_OPERATOR catalog with minimal
 * information about the operator (just its name and types).
 * Forward declaration is used only for this purpose, it is
 * not available to the user as it is for type definition.
 */
ObjectAddress
OperatorCreate(const char *operatorName,
			   Oid operatorNamespace,
			   Oid leftTypeId,
			   Oid rightTypeId,
			   Oid procedureId,
			   List *commutatorName,
			   List *negatorName,
			   Oid restrictionId,
			   Oid joinId,
			   bool canMerge,
			   bool canHash)
{
	Relation	pg_operator_desc;
	HeapTuple	tup;
	bool		isUpdate;
	bool		nulls[Natts_pg_operator];
	bool		replaces[Natts_pg_operator];
	Datum		values[Natts_pg_operator];
	Oid			operatorObjectId;
	bool		operatorAlreadyDefined;
	Oid			operResultType;
	Oid			commutatorId,
				negatorId;
	bool		selfCommutator = false;
	NameData	oname;
	int			i;
	ObjectAddress address;

	/*
	 * Sanity checks
	 */
	if (!validOperatorName(operatorName))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("\"%s\" is not a valid operator name",
						operatorName)));

	if (!(OidIsValid(leftTypeId) && OidIsValid(rightTypeId)))
	{
		/* If it's not a binary op, these things mustn't be set: */
		if (commutatorName)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can have commutators")));
		if (OidIsValid(joinId))
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

	operResultType = get_func_rettype(procedureId);

	if (operResultType != BOOLOID)
	{
		/* If it's not a boolean op, these things mustn't be set: */
		if (negatorName)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have negators")));
		if (OidIsValid(restrictionId))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have restriction selectivity")));
		if (OidIsValid(joinId))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				errmsg("only boolean operators can have join selectivity")));
		if (canMerge)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can merge join")));
		if (canHash)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can hash")));
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
	 * filling in a previously-created shell.  Insist that the user own any
	 * such shell.
	 */
	if (OidIsValid(operatorObjectId) &&
		!pg_oper_ownercheck(operatorObjectId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPER,
					   operatorName);

	/*
	 * Set up the other operators.  If they do not currently exist, create
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

		/* Permission check: must own other operator */
		if (OidIsValid(commutatorId) &&
			!pg_oper_ownercheck(commutatorId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPER,
						   NameListToString(commutatorName));

		/*
		 * self-linkage to this operator; will fix below. Note that only
		 * self-linkage for commutation makes sense.
		 */
		if (!OidIsValid(commutatorId))
			selfCommutator = true;
	}
	else
		commutatorId = InvalidOid;

	if (negatorName)
	{
		/* negator has same arg types */
		negatorId = get_other_operator(negatorName,
									   leftTypeId, rightTypeId,
									   operatorName, operatorNamespace,
									   leftTypeId, rightTypeId,
									   false);

		/* Permission check: must own other operator */
		if (OidIsValid(negatorId) &&
			!pg_oper_ownercheck(negatorId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_OPER,
						   NameListToString(negatorName));
	}
	else
		negatorId = InvalidOid;

	/*
	 * set up values in the operator tuple
	 */

	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) NULL;
		replaces[i] = true;
		nulls[i] = false;
	}

	namestrcpy(&oname, operatorName);
	values[Anum_pg_operator_oprname - 1] = NameGetDatum(&oname);
	values[Anum_pg_operator_oprnamespace - 1] = ObjectIdGetDatum(operatorNamespace);
	values[Anum_pg_operator_oprowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_operator_oprkind - 1] = CharGetDatum(leftTypeId ? (rightTypeId ? 'b' : 'r') : 'l');
	values[Anum_pg_operator_oprcanmerge - 1] = BoolGetDatum(canMerge);
	values[Anum_pg_operator_oprcanhash - 1] = BoolGetDatum(canHash);
	values[Anum_pg_operator_oprleft - 1] = ObjectIdGetDatum(leftTypeId);
	values[Anum_pg_operator_oprright - 1] = ObjectIdGetDatum(rightTypeId);
	values[Anum_pg_operator_oprresult - 1] = ObjectIdGetDatum(operResultType);
	values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(commutatorId);
	values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(negatorId);
	values[Anum_pg_operator_oprcode - 1] = ObjectIdGetDatum(procedureId);
	values[Anum_pg_operator_oprrest - 1] = ObjectIdGetDatum(restrictionId);
	values[Anum_pg_operator_oprjoin - 1] = ObjectIdGetDatum(joinId);

	pg_operator_desc = heap_open(OperatorRelationId, RowExclusiveLock);

	/*
	 * If we are replacing an operator shell, update; else insert
	 */
	if (operatorObjectId)
	{
		isUpdate = true;

		tup = SearchSysCacheCopy1(OPEROID,
								  ObjectIdGetDatum(operatorObjectId));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u",
				 operatorObjectId);

		tup = heap_modify_tuple(tup,
								RelationGetDescr(pg_operator_desc),
								values,
								nulls,
								replaces);

		simple_heap_update(pg_operator_desc, &tup->t_self, tup);
	}
	else
	{
		isUpdate = false;

		tup = heap_form_tuple(RelationGetDescr(pg_operator_desc),
							  values, nulls);

		operatorObjectId = simple_heap_insert(pg_operator_desc, tup);
	}

	/* Must update the indexes in either case */
	CatalogUpdateIndexes(pg_operator_desc, tup);

	/* Add dependencies for the entry */
	address = makeOperatorDependencies(tup, isUpdate);

	/* Post creation hook for new operator */
	InvokeObjectPostCreateHook(OperatorRelationId, operatorObjectId, 0);

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
		OperatorUpd(operatorObjectId, commutatorId, negatorId, false);

	return address;
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
 *	When isDelete is false, update their negator and commutator fields to
 *	point back to the given operator; when isDelete is true, update those
 *	fields to no longer point back to the given operator.
 *
 *	The !isDelete case solves a problem for users who need to insert two new
 *	operators that are the negator or commutator of each other, while the
 *	isDelete case is needed so as not to leave dangling OID links behind
 *	after dropping an operator.
 */
void
OperatorUpd(Oid baseId, Oid commId, Oid negId, bool isDelete)
{
	Relation	pg_operator_desc;
	HeapTuple	tup;

	/*
	 * If we're making an operator into its own commutator, then we need a
	 * command-counter increment here, since we've just inserted the tuple
	 * we're about to update.  But when we're dropping an operator, we can
	 * skip this because we're at the beginning of the command.
	 */
	if (!isDelete)
		CommandCounterIncrement();

	/* Open the relation. */
	pg_operator_desc = heap_open(OperatorRelationId, RowExclusiveLock);

	/* Get a writable copy of the commutator's tuple. */
	if (OidIsValid(commId))
		tup = SearchSysCacheCopy1(OPEROID, ObjectIdGetDatum(commId));
	else
		tup = NULL;

	/* Update the commutator's tuple if need be. */
	if (HeapTupleIsValid(tup))
	{
		Form_pg_operator t = (Form_pg_operator) GETSTRUCT(tup);
		bool		update_commutator = false;

		/*
		 * Out of due caution, we only change the commutator's oprcom field if
		 * it has the exact value we expected: InvalidOid when creating an
		 * operator, or baseId when dropping one.
		 */
		if (isDelete && t->oprcom == baseId)
		{
			t->oprcom = InvalidOid;
			update_commutator = true;
		}
		else if (!isDelete && !OidIsValid(t->oprcom))
		{
			t->oprcom = baseId;
			update_commutator = true;
		}

		/* If any columns were found to need modification, update tuple. */
		if (update_commutator)
		{
			simple_heap_update(pg_operator_desc, &tup->t_self, tup);
			CatalogUpdateIndexes(pg_operator_desc, tup);

			/*
			 * Do CCI to make the updated tuple visible.  We must do this in
			 * case the commutator is also the negator.  (Which would be a
			 * logic error on the operator definer's part, but that's not a
			 * good reason to fail here.)  We would need a CCI anyway in the
			 * deletion case for a self-commutator with no negator.
			 */
			CommandCounterIncrement();
		}
	}

	/*
	 * Similarly find and update the negator, if any.
	 */
	if (OidIsValid(negId))
		tup = SearchSysCacheCopy1(OPEROID, ObjectIdGetDatum(negId));
	else
		tup = NULL;

	if (HeapTupleIsValid(tup))
	{
		Form_pg_operator t = (Form_pg_operator) GETSTRUCT(tup);
		bool		update_negator = false;

		/*
		 * Out of due caution, we only change the negator's oprnegate field if
		 * it has the exact value we expected: InvalidOid when creating an
		 * operator, or baseId when dropping one.
		 */
		if (isDelete && t->oprnegate == baseId)
		{
			t->oprnegate = InvalidOid;
			update_negator = true;
		}
		else if (!isDelete && !OidIsValid(t->oprnegate))
		{
			t->oprnegate = baseId;
			update_negator = true;
		}

		/* If any columns were found to need modification, update tuple. */
		if (update_negator)
		{
			simple_heap_update(pg_operator_desc, &tup->t_self, tup);
			CatalogUpdateIndexes(pg_operator_desc, tup);

			/*
			 * In the deletion case, do CCI to make the updated tuple visible.
			 * We must do this in case the operator is its own negator. (Which
			 * would be a logic error on the operator definer's part, but
			 * that's not a good reason to fail here.)
			 */
			if (isDelete)
				CommandCounterIncrement();
		}
	}

	/* Close relation and release catalog lock. */
	heap_close(pg_operator_desc, RowExclusiveLock);
}

/*
 * Create dependencies for an operator (either a freshly inserted
 * complete operator, a new shell operator, a just-updated shell,
 * or an operator that's being modified by ALTER OPERATOR).
 *
 * NB: the OidIsValid tests in this routine are necessary, in case
 * the given operator is a shell.
 */
ObjectAddress
makeOperatorDependencies(HeapTuple tuple, bool isUpdate)
{
	Form_pg_operator oper = (Form_pg_operator) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;

	myself.classId = OperatorRelationId;
	myself.objectId = HeapTupleGetOid(tuple);
	myself.objectSubId = 0;

	/*
	 * If we are updating the operator, delete any existing entries, except
	 * for extension membership which should remain the same.
	 */
	if (isUpdate)
	{
		deleteDependencyRecordsFor(myself.classId, myself.objectId, true);
		deleteSharedDependencyRecordsFor(myself.classId, myself.objectId, 0);
	}

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

	/* Dependency on extension */
	recordDependencyOnCurrentExtension(&myself, true);

	return myself;
}
