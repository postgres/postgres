/*-------------------------------------------------------------------------
 *
 * pg_operator.c
 *	  routines to support manipulation of the pg_operator relation
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_operator.c,v 1.35 1999/04/23 00:50:57 tgl Exp $
 *
 * NOTES
 *	  these routines moved here from commands/define.c and somewhat cleaned up.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

static Oid OperatorGetWithOpenRelation(Relation pg_operator_desc,
									   const char *operatorName,
									   Oid leftObjectId,
									   Oid rightObjectId,
									   bool *defined);

static Oid OperatorGet(char *operatorName,
					   char *leftTypeName,
					   char *rightTypeName,
					   bool *defined);

static Oid OperatorShellMakeWithOpenRelation(Relation pg_operator_desc,
								  char *operatorName,
								  Oid leftObjectId,
								  Oid rightObjectId);

static Oid OperatorShellMake(char *operatorName,
				  char *leftTypeName,
				  char *rightTypeName);

static void OperatorDef(char *operatorName,
			char *leftTypeName,
			char *rightTypeName,
			char *procedureName,
			uint16 precedence,
			bool isLeftAssociative,
			char *commutatorName,
			char *negatorName,
			char *restrictionName,
			char *oinName,
			bool canHash,
			char *leftSortName,
			char *rightSortName);

static void OperatorUpd(Oid baseId, Oid commId, Oid negId);

/* ----------------------------------------------------------------
 *		OperatorGetWithOpenRelation
 *
 *		preforms a scan on pg_operator for an operator tuple
 *		with given name and left/right type oids.
 * ----------------------------------------------------------------
 *	  pg_operator_desc	-- reldesc for pg_operator
 *	  operatorName		-- name of operator to fetch
 *	  leftObjectId		-- left data type oid of operator to fetch
 *	  rightObjectId		-- right data type oid of operator to fetch
 *	  defined			-- set TRUE if defined (not a shell)
 */
static Oid
OperatorGetWithOpenRelation(Relation pg_operator_desc,
							const char *operatorName,
							Oid leftObjectId,
							Oid rightObjectId,
							bool *defined)
{
	HeapScanDesc pg_operator_scan;
	Oid			operatorObjectId;
	HeapTuple	tup;

	static ScanKeyData opKey[3] = {
		{0, Anum_pg_operator_oprname, F_NAMEEQ},
		{0, Anum_pg_operator_oprleft, F_OIDEQ},
		{0, Anum_pg_operator_oprright, F_OIDEQ},
	};

	fmgr_info(F_NAMEEQ, &opKey[0].sk_func);
	fmgr_info(F_OIDEQ, &opKey[1].sk_func);
	fmgr_info(F_OIDEQ, &opKey[2].sk_func);
	opKey[0].sk_nargs = opKey[0].sk_func.fn_nargs;
	opKey[1].sk_nargs = opKey[1].sk_func.fn_nargs;
	opKey[2].sk_nargs = opKey[2].sk_func.fn_nargs;

	/* ----------------
	 *	form scan key
	 * ----------------
	 */
	opKey[0].sk_argument = PointerGetDatum(operatorName);
	opKey[1].sk_argument = ObjectIdGetDatum(leftObjectId);
	opKey[2].sk_argument = ObjectIdGetDatum(rightObjectId);

	/* ----------------
	 *	begin the scan
	 * ----------------
	 */
	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* no cache? */
									  3,
									  opKey);

	/* ----------------
	 *	fetch the operator tuple, if it exists, and determine
	 *	the proper return oid value.
	 * ----------------
	 */
	tup = heap_getnext(pg_operator_scan, 0);

	if (HeapTupleIsValid(tup))
	{
		regproc		oprcode = ((Form_pg_operator) GETSTRUCT(tup))->oprcode;
		operatorObjectId = tup->t_data->t_oid;
		*defined = RegProcedureIsValid(oprcode);
	}
	else
	{
		operatorObjectId = InvalidOid;
		*defined = false;
	}

	/* ----------------
	 *	close the scan and return the oid.
	 * ----------------
	 */
	heap_endscan(pg_operator_scan);

	return operatorObjectId;
}

/* ----------------------------------------------------------------
 *		OperatorGet
 *
 *		finds the operator associated with the specified name
 *		and left and right type names.
 * ----------------------------------------------------------------
 */
static Oid
OperatorGet(char *operatorName,
			char *leftTypeName,
			char *rightTypeName,
			bool *defined)
{
	Relation	pg_operator_desc;

	Oid			operatorObjectId;
	Oid			leftObjectId = InvalidOid;
	Oid			rightObjectId = InvalidOid;
	bool		leftDefined = false;
	bool		rightDefined = false;

	/* ----------------
	 *	look up the operator data types.
	 *
	 *	Note: types must be defined before operators
	 * ----------------
	 */
	if (leftTypeName)
	{
		leftObjectId = TypeGet(leftTypeName, &leftDefined);

		if (!OidIsValid(leftObjectId) || !leftDefined)
			elog(ERROR, "OperatorGet: left type '%s' nonexistent",
				 leftTypeName);
	}

	if (rightTypeName)
	{
		rightObjectId = TypeGet(rightTypeName, &rightDefined);

		if (!OidIsValid(rightObjectId) || !rightDefined)
			elog(ERROR, "OperatorGet: right type '%s' nonexistent",
				 rightTypeName);
	}

	if (!((OidIsValid(leftObjectId) && leftDefined) ||
		  (OidIsValid(rightObjectId) && rightDefined)))
		elog(ERROR, "OperatorGet: must have at least one argument type");

	/* ----------------
	 *	open the pg_operator relation
	 * ----------------
	 */
	pg_operator_desc = heap_openr(OperatorRelationName);

	/* ----------------
	 *	get the oid for the operator with the appropriate name
	 *	and left/right types.
	 * ----------------
	 */
	operatorObjectId = OperatorGetWithOpenRelation(pg_operator_desc,
												   operatorName,
												   leftObjectId,
												   rightObjectId,
												   defined);

	/* ----------------
	 *	close the relation and return the operator oid.
	 * ----------------
	 */
	heap_close(pg_operator_desc);

	return operatorObjectId;
}

/* ----------------------------------------------------------------
 *		OperatorShellMakeWithOpenRelation
 *
 * ----------------------------------------------------------------
 */
static Oid
OperatorShellMakeWithOpenRelation(Relation pg_operator_desc,
								  char *operatorName,
								  Oid leftObjectId,
								  Oid rightObjectId)
{
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_operator];
	char		nulls[Natts_pg_operator];
	Oid			operatorObjectId;
	NameData	oname;
	TupleDesc	tupDesc;

	/* ----------------
	 *	initialize our *nulls and *values arrays
	 * ----------------
	 */
	for (i = 0; i < Natts_pg_operator; ++i)
	{
		nulls[i] = ' ';
		values[i] = (Datum) NULL;		/* redundant, but safe */
	}

	/* ----------------
	 *	initialize *values with the operator name and input data types.
	 *  Note that oprcode is set to InvalidOid, indicating it's a shell.
	 * ----------------
	 */
	i = 0;
	namestrcpy(&oname, operatorName);
	values[i++] = NameGetDatum(&oname);
	values[i++] = Int32GetDatum(GetUserId());
	values[i++] = (Datum) (uint16) 0;
	values[i++] = (Datum) 'b';	/* assume it's binary */
	values[i++] = (Datum) (bool) 0;
	values[i++] = (Datum) (bool) 0;
	values[i++] = ObjectIdGetDatum(leftObjectId);		/* <-- left oid */
	values[i++] = ObjectIdGetDatum(rightObjectId);		/* <-- right oid */
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);
	values[i++] = ObjectIdGetDatum(InvalidOid);

	/* ----------------
	 *	create a new operator tuple
	 * ----------------
	 */
	tupDesc = pg_operator_desc->rd_att;

	tup = heap_formtuple(tupDesc,
						 values,
						 nulls);

	/* ----------------
	 *	insert our "shell" operator tuple and
	 *	close the relation
	 * ----------------
	 */
	heap_insert(pg_operator_desc, tup);
	operatorObjectId = tup->t_data->t_oid;

	/* ----------------
	 *	free the tuple and return the operator oid
	 * ----------------
	 */
	pfree(tup);

	return operatorObjectId;
}

/* ----------------------------------------------------------------
 *		OperatorShellMake
 *
 *		Specify operator name and left and right type names,
 *		fill an operator struct with this info and NULL's,
 *		call heap_insert and return the Oid
 *		to the caller.
 * ----------------------------------------------------------------
 */
static Oid
OperatorShellMake(char *operatorName,
				  char *leftTypeName,
				  char *rightTypeName)
{
	Relation	pg_operator_desc;
	Oid			operatorObjectId;

	Oid			leftObjectId = InvalidOid;
	Oid			rightObjectId = InvalidOid;
	bool		leftDefined = false;
	bool		rightDefined = false;

	/* ----------------
	 *	get the left and right type oid's for this operator
	 * ----------------
	 */
	if (leftTypeName)
		leftObjectId = TypeGet(leftTypeName, &leftDefined);

	if (rightTypeName)
		rightObjectId = TypeGet(rightTypeName, &rightDefined);

	if (!((OidIsValid(leftObjectId) && leftDefined) ||
		  (OidIsValid(rightObjectId) && rightDefined)))
		elog(ERROR, "OperatorShellMake: no valid argument types??");

	/* ----------------
	 *	open pg_operator
	 * ----------------
	 */
	pg_operator_desc = heap_openr(OperatorRelationName);

	/* ----------------
	 *	add a "shell" operator tuple to the operator relation
	 *	and recover the shell tuple's oid.
	 * ----------------
	 */
	operatorObjectId = OperatorShellMakeWithOpenRelation(pg_operator_desc,
										  operatorName,
										  leftObjectId,
										  rightObjectId);
	/* ----------------
	 *	close the operator relation and return the oid.
	 * ----------------
	 */
	heap_close(pg_operator_desc);

	return operatorObjectId;
}

/* --------------------------------
 * OperatorDef
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
 *	 precedence
 *	 operator "kind" either "b" for binary or "l" for left unary
 *	 isLeftAssociative boolean
 *	 canHash boolean
 *	 leftTypeObjectId -- type must already be defined
 *	 rightTypeObjectId -- this is optional, enter ObjectId=0 if none specified
 *	 resultType -- defer this, since it must be determined from
 *				   the pg_procedure catalog
 *	 commutatorObjectId -- if this is NULL, enter ObjectId=0
 *					  else if this already exists, enter it's ObjectId
 *					  else if this does not yet exist, and is not
 *						the same as the main operatorName, then create
 *						a shell and enter the new ObjectId
 *					  else if this does not exist but IS the same
 *						name & types as the main operator, set the ObjectId=0.
 *						(We are creating a self-commutating operator.)
 *						The link will be fixed later by OperatorUpd.
 *	 negatorObjectId   -- same as for commutatorObjectId
 *	 leftSortObjectId  -- same as for commutatorObjectId
 *	 rightSortObjectId -- same as for commutatorObjectId
 *	 operatorProcedure -- must access the pg_procedure catalog to get the
 *				   ObjectId of the procedure that actually does the operator
 *				   actions this is required.  Do an amgetattr to find out the
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
 *	 call heap_insert
 * --------------------------------
 *		"X" indicates an optional argument (i.e. one that can be NULL)
 *		operatorName;			-- operator name
 *		leftTypeName;			-- X left type name
 *		rightTypeName;			-- X right type name
 *		procedureName;			-- procedure name for operator code
 *		precedence;				-- operator precedence
 *		isLeftAssociative;		-- operator is left associative?
 *		commutatorName;			-- X commutator operator name
 *		negatorName;			-- X negator operator name
 *		restrictionName;		-- X restriction sel. procedure name
 *		joinName;				-- X join sel. procedure name
 *		canHash;				-- can hash join be used with operator?
 *		leftSortName;			-- X left sort operator (for merge join)
 *		rightSortName;			-- X right sort operator (for merge join)
 */
static void
OperatorDef(char *operatorName,
			char *leftTypeName,
			char *rightTypeName,
			char *procedureName,
			uint16 precedence,
			bool isLeftAssociative,
			char *commutatorName,
			char *negatorName,
			char *restrictionName,
			char *joinName,
			bool canHash,
			char *leftSortName,
			char *rightSortName)
{
	int			i,
				j;
	Relation	pg_operator_desc;

	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	char		nulls[Natts_pg_operator];
	char		replaces[Natts_pg_operator];
	Datum		values[Natts_pg_operator];
	Oid			operatorObjectId;
	bool		operatorAlreadyDefined;
	Oid			leftTypeId = InvalidOid;
	Oid			rightTypeId = InvalidOid;
	Oid			commutatorId = InvalidOid;
	Oid			negatorId = InvalidOid;
	bool		leftDefined = false;
	bool		rightDefined = false;
	bool		selfCommutator = false;
	char	   *name[4];
	Oid			typeId[8];
	int			nargs;
	NameData	oname;
	TupleDesc	tupDesc;

	static ScanKeyData opKey[3] = {
		{0, Anum_pg_operator_oprname, F_NAMEEQ},
		{0, Anum_pg_operator_oprleft, F_OIDEQ},
		{0, Anum_pg_operator_oprright, F_OIDEQ},
	};

	fmgr_info(F_NAMEEQ, &opKey[0].sk_func);
	fmgr_info(F_OIDEQ, &opKey[1].sk_func);
	fmgr_info(F_OIDEQ, &opKey[2].sk_func);
	opKey[0].sk_nargs = opKey[0].sk_func.fn_nargs;
	opKey[1].sk_nargs = opKey[1].sk_func.fn_nargs;
	opKey[2].sk_nargs = opKey[2].sk_func.fn_nargs;

	operatorObjectId = OperatorGet(operatorName,
								   leftTypeName,
								   rightTypeName,
								   &operatorAlreadyDefined);

	if (operatorAlreadyDefined)
		elog(ERROR, "OperatorDef: operator \"%s\" already defined",
			 operatorName);

	/* At this point, if operatorObjectId is not InvalidOid then
	 * we are filling in a previously-created shell.
	 */

	/* ----------------
	 *	look up the operator data types.
	 *
	 *	Note: types must be defined before operators
	 * ----------------
	 */
	if (leftTypeName)
	{
		leftTypeId = TypeGet(leftTypeName, &leftDefined);

		if (!OidIsValid(leftTypeId) || !leftDefined)
			elog(ERROR, "OperatorDef: left type '%s' nonexistent",
				 leftTypeName);
	}

	if (rightTypeName)
	{
		rightTypeId = TypeGet(rightTypeName, &rightDefined);

		if (!OidIsValid(rightTypeId) || !rightDefined)
			elog(ERROR, "OperatorDef: right type '%s' nonexistent",
				 rightTypeName);
	}

	if (!((OidIsValid(leftTypeId) && leftDefined) ||
		  (OidIsValid(rightTypeId) && rightDefined)))
		elog(ERROR, "OperatorDef: must have at least one argument type");

	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) NULL;
		replaces[i] = 'r';
		nulls[i] = ' ';
	}

	/* ----------------
	 * Look up registered procedures -- find the return type
	 * of procedureName to place in "result" field.
	 * Do this before shells are created so we don't
	 * have to worry about deleting them later.
	 * ----------------
	 */
	MemSet(typeId, 0, 8 * sizeof(Oid));
	if (!leftTypeName)
	{
		typeId[0] = rightTypeId;
		nargs = 1;
	}
	else if (!rightTypeName)
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
	tup = SearchSysCacheTuple(PRONAME,
							  PointerGetDatum(procedureName),
							  Int32GetDatum(nargs),
							  PointerGetDatum(typeId),
							  0);

	if (!HeapTupleIsValid(tup))
		func_error("OperatorDef", procedureName, nargs, typeId, NULL);

	values[Anum_pg_operator_oprcode - 1] = ObjectIdGetDatum(tup->t_data->t_oid);
	values[Anum_pg_operator_oprresult - 1] = ObjectIdGetDatum(((Form_pg_proc)
						  GETSTRUCT(tup))->prorettype);

	/* ----------------
	 *	find restriction
	 * ----------------
	 */
	if (restrictionName)
	{							/* optional */
		MemSet(typeId, 0, 8 * sizeof(Oid));
		typeId[0] = OIDOID;		/* operator OID */
		typeId[1] = OIDOID;		/* relation OID */
		typeId[2] = INT2OID;	/* attribute number */
		typeId[3] = 0;			/* value - can be any type	*/
		typeId[4] = INT4OID;	/* flags - left or right selectivity */
		tup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(restrictionName),
								  Int32GetDatum(5),
								  PointerGetDatum(typeId),
								  0);
		if (!HeapTupleIsValid(tup))
			func_error("OperatorDef", restrictionName, 5, typeId, NULL);

		values[Anum_pg_operator_oprrest - 1] = ObjectIdGetDatum(tup->t_data->t_oid);
	}
	else
		values[Anum_pg_operator_oprrest - 1] = ObjectIdGetDatum(InvalidOid);

	/* ----------------
	 *	find join - only valid for binary operators
	 * ----------------
	 */
	if (joinName)
	{							/* optional */
		MemSet(typeId, 0, 8 * sizeof(Oid));
		typeId[0] = OIDOID;		/* operator OID */
		typeId[1] = OIDOID;		/* relation OID 1 */
		typeId[2] = INT2OID;	/* attribute number 1 */
		typeId[3] = OIDOID;		/* relation OID 2 */
		typeId[4] = INT2OID;	/* attribute number 2 */

		tup = SearchSysCacheTuple(PRONAME,
								  PointerGetDatum(joinName),
								  Int32GetDatum(5),
								  PointerGetDatum(typeId),
								  0);
		if (!HeapTupleIsValid(tup))
			func_error("OperatorDef", joinName, 5, typeId, NULL);

		values[Anum_pg_operator_oprjoin - 1] = ObjectIdGetDatum(tup->t_data->t_oid);
	}
	else
		values[Anum_pg_operator_oprjoin - 1] = ObjectIdGetDatum(InvalidOid);

	/* ----------------
	 * set up values in the operator tuple
	 * ----------------
	 */
	i = 0;
	namestrcpy(&oname, operatorName);
	values[i++] = NameGetDatum(&oname);
	values[i++] = Int32GetDatum(GetUserId());
	values[i++] = UInt16GetDatum(precedence);
	values[i++] = leftTypeName ? (rightTypeName ? 'b' : 'r') : 'l';
	values[i++] = Int8GetDatum(isLeftAssociative);
	values[i++] = Int8GetDatum(canHash);
	values[i++] = ObjectIdGetDatum(leftTypeId);
	values[i++] = ObjectIdGetDatum(rightTypeId);

	++i;						/* Skip "oprresult", it was filled in above */

	/*
	 * Set up the other operators.	If they do not currently exist, create
	 * shells in order to get ObjectId's.
	 */
	name[0] = commutatorName;
	name[1] = negatorName;
	name[2] = leftSortName;
	name[3] = rightSortName;

	for (j = 0; j < 4; ++j)
	{
		if (name[j])
		{
			char   *otherLeftTypeName = NULL;
			char   *otherRightTypeName = NULL;
			Oid		otherLeftTypeId = InvalidOid;
			Oid		otherRightTypeId = InvalidOid;
			Oid		other_oid = InvalidOid;
			bool	otherDefined = false;

			switch (j)
			{
				case 0:			/* commutator has reversed arg types */
					otherLeftTypeName = rightTypeName;
					otherRightTypeName = leftTypeName;
					otherLeftTypeId = rightTypeId;
					otherRightTypeId = leftTypeId;
					other_oid = OperatorGet(name[j],
											otherLeftTypeName,
											otherRightTypeName,
											&otherDefined);
					commutatorId = other_oid;
					break;
				case 1:			/* negator has same arg types */
					otherLeftTypeName = leftTypeName;
					otherRightTypeName = rightTypeName;
					otherLeftTypeId = leftTypeId;
					otherRightTypeId = rightTypeId;
					other_oid = OperatorGet(name[j],
											otherLeftTypeName,
											otherRightTypeName,
											&otherDefined);
					negatorId = other_oid;
					break;
				case 2:			/* left sort op takes left-side data type */
					otherLeftTypeName = leftTypeName;
					otherRightTypeName = leftTypeName;
					otherLeftTypeId = leftTypeId;
					otherRightTypeId = leftTypeId;
					other_oid = OperatorGet(name[j],
											otherLeftTypeName,
											otherRightTypeName,
											&otherDefined);
					break;
				case 3:			/* right sort op takes right-side data type */
					otherLeftTypeName = rightTypeName;
					otherRightTypeName = rightTypeName;
					otherLeftTypeId = rightTypeId;
					otherRightTypeId = rightTypeId;
					other_oid = OperatorGet(name[j],
											otherLeftTypeName,
											otherRightTypeName,
											&otherDefined);
					break;
			}

			if (OidIsValid(other_oid))
			{
				/* other op already in catalogs */
				values[i++] = ObjectIdGetDatum(other_oid);
			}
			else if (strcmp(operatorName, name[j]) != 0 ||
					 otherLeftTypeId != leftTypeId ||
					 otherRightTypeId != rightTypeId)
			{
				/* not in catalogs, different from operator */
				other_oid = OperatorShellMake(name[j],
											  otherLeftTypeName,
											  otherRightTypeName);
				if (!OidIsValid(other_oid))
					elog(ERROR,
						 "OperatorDef: can't create operator shell '%s'",
						 name[j]);
				values[i++] = ObjectIdGetDatum(other_oid);
			}
			else
			{
				/* self-linkage to this operator; will fix below.
				 * Note that only self-linkage for commutation makes sense.
				 */
				if (j != 0)
					elog(ERROR,
						 "OperatorDef: operator can't be its own negator or sort op");
				selfCommutator = true;
				values[i++] = ObjectIdGetDatum(InvalidOid);
			}
		}
		else
		{
			/* other operator is omitted */
			values[i++] = ObjectIdGetDatum(InvalidOid);
		}
	}

	/* last three fields were filled in above */

	/*
	 * If we are adding to an operator shell, get its t_self
	 */
	pg_operator_desc = heap_openr(OperatorRelationName);

	if (operatorObjectId)
	{
		opKey[0].sk_argument = PointerGetDatum(operatorName);
		opKey[1].sk_argument = ObjectIdGetDatum(leftTypeId);
		opKey[2].sk_argument = ObjectIdGetDatum(rightTypeId);

		pg_operator_scan = heap_beginscan(pg_operator_desc,
										  0,
										  SnapshotSelf, /* no cache? */
										  3,
										  opKey);

		tup = heap_getnext(pg_operator_scan, 0);
		if (HeapTupleIsValid(tup))
		{
			tup = heap_modifytuple(tup,
								   pg_operator_desc,
								   values,
								   nulls,
								   replaces);

			setheapoverride(true);
			heap_replace(pg_operator_desc, &tup->t_self, tup, NULL);
			setheapoverride(false);
		}
		else
			elog(ERROR, "OperatorDef: no operator %d", operatorObjectId);

		heap_endscan(pg_operator_scan);
	}
	else
	{
		tupDesc = pg_operator_desc->rd_att;
		tup = heap_formtuple(tupDesc, values, nulls);

		heap_insert(pg_operator_desc, tup);
		operatorObjectId = tup->t_data->t_oid;
	}

	heap_close(pg_operator_desc);

	/*
	 * If a commutator and/or negator link is provided, update the other
	 * operator(s) to point at this one, if they don't already have a link.
	 * This supports an alternate style of operator definition wherein the
	 * user first defines one operator without giving negator or
	 * commutator, then defines the other operator of the pair with the
	 * proper commutator or negator attribute.  That style doesn't require
	 * creation of a shell, and it's the only style that worked right before
	 * Postgres version 6.5.
	 * This code also takes care of the situation where the new operator
	 * is its own commutator.
	 */
	if (selfCommutator)
		commutatorId = operatorObjectId;

	if (OidIsValid(commutatorId) || OidIsValid(negatorId))
		OperatorUpd(operatorObjectId, commutatorId, negatorId);
}

/* ----------------------------------------------------------------
 * OperatorUpd
 *
 *	For a given operator, look up its negator and commutator operators.
 *	If they are defined, but their negator and commutator fields
 *	(respectively) are empty, then use the new operator for neg or comm.
 *	This solves a problem for users who need to insert two new operators
 *	which are the negator or commutator of each other.
 * ----------------------------------------------------------------
 */
static void
OperatorUpd(Oid baseId, Oid commId, Oid negId)
{
	int			i;
	Relation	pg_operator_desc;
	HeapScanDesc pg_operator_scan;
	HeapTuple	tup;
	char		nulls[Natts_pg_operator];
	char		replaces[Natts_pg_operator];
	Datum		values[Natts_pg_operator];

	static ScanKeyData opKey[1] = {
		{0, ObjectIdAttributeNumber, F_OIDEQ},
	};

	fmgr_info(F_OIDEQ, &opKey[0].sk_func);
	opKey[0].sk_nargs = opKey[0].sk_func.fn_nargs;

	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) NULL;
		replaces[i] = ' ';
		nulls[i] = ' ';
	}

	pg_operator_desc = heap_openr(OperatorRelationName);

	/* check and update the commutator, if necessary */
	opKey[0].sk_argument = ObjectIdGetDatum(commId);

	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* no cache? */
									  1,
									  opKey);

	tup = heap_getnext(pg_operator_scan, 0);

	/* if the commutator and negator are the same operator, do one update.
	 * XXX this is probably useless code --- I doubt it ever makes sense
	 * for commutator and negator to be the same thing...
	 */
	if (commId == negId)
	{
		if (HeapTupleIsValid(tup))
		{
			Form_pg_operator t;

			t = (Form_pg_operator) GETSTRUCT(tup);
			if (!OidIsValid(t->oprcom)
				|| !OidIsValid(t->oprnegate))
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
									   pg_operator_desc,
									   values,
									   nulls,
									   replaces);

				setheapoverride(true);
				heap_replace(pg_operator_desc, &tup->t_self, tup, NULL);
				setheapoverride(false);

			}
		}
		heap_endscan(pg_operator_scan);

		heap_close(pg_operator_desc);

		return;
	}

	/* if commutator and negator are different, do two updates */

	if (HeapTupleIsValid(tup) &&
		!(OidIsValid(((Form_pg_operator) GETSTRUCT(tup))->oprcom)))
	{
		values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(baseId);
		replaces[Anum_pg_operator_oprcom - 1] = 'r';
		tup = heap_modifytuple(tup,
							   pg_operator_desc,
							   values,
							   nulls,
							   replaces);

		setheapoverride(true);
		heap_replace(pg_operator_desc, &tup->t_self, tup, NULL);
		setheapoverride(false);

		values[Anum_pg_operator_oprcom - 1] = (Datum) NULL;
		replaces[Anum_pg_operator_oprcom - 1] = ' ';
	}

	heap_endscan(pg_operator_scan);

	/* check and update the negator, if necessary */
	opKey[0].sk_argument = ObjectIdGetDatum(negId);

	pg_operator_scan = heap_beginscan(pg_operator_desc,
									  0,
									  SnapshotSelf,		/* no cache? */
									  1,
									  opKey);

	tup = heap_getnext(pg_operator_scan, 0);
	if (HeapTupleIsValid(tup) &&
		!(OidIsValid(((Form_pg_operator) GETSTRUCT(tup))->oprnegate)))
	{
		values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(baseId);
		replaces[Anum_pg_operator_oprnegate - 1] = 'r';
		tup = heap_modifytuple(tup,
							   pg_operator_desc,
							   values,
							   nulls,
							   replaces);

		setheapoverride(true);
		heap_replace(pg_operator_desc, &tup->t_self, tup, NULL);
		setheapoverride(false);
	}

	heap_endscan(pg_operator_scan);

	heap_close(pg_operator_desc);
}


/* ----------------------------------------------------------------
 * OperatorCreate
 *
 * This is now just an interface procedure for OperatorDef ...
 *
 * "X" indicates an optional argument (i.e. one that can be NULL)
 *		operatorName;			-- operator name
 *		leftTypeName;			-- X left type name
 *		rightTypeName;			-- X right type name
 *		procedureName;			-- procedure for operator
 *		precedence;				-- operator precedence
 *		isLeftAssociative;		-- operator is left associative
 *		commutatorName;			-- X commutator operator name
 *		negatorName;			-- X negator operator name
 *		restrictionName;		-- X restriction sel. procedure
 *		joinName;				-- X join sel. procedure
 *		canHash;				-- hash join can be used with this operator
 *		leftSortName;			-- X left sort operator (for merge join)
 *		rightSortName;			-- X right sort operator (for merge join)
 */
void
OperatorCreate(char *operatorName,
			   char *leftTypeName,
			   char *rightTypeName,
			   char *procedureName,
			   uint16 precedence,
			   bool isLeftAssociative,
			   char *commutatorName,
			   char *negatorName,
			   char *restrictionName,
			   char *joinName,
			   bool canHash,
			   char *leftSortName,
			   char *rightSortName)
{
	if (!leftTypeName && !rightTypeName)
		elog(ERROR, "OperatorCreate: at least one of leftarg or rightarg must be defined");

	if (! (leftTypeName && rightTypeName))
	{
		/* If it's not a binary op, these things mustn't be set: */
		if (commutatorName)
			elog(ERROR, "OperatorCreate: only binary operators can have commutators");
		if (negatorName)
			elog(ERROR, "OperatorCreate: only binary operators can have negators");
		if (restrictionName || joinName)
			elog(ERROR, "OperatorCreate: only binary operators can have selectivity");
		if (canHash)
			elog(ERROR, "OperatorCreate: only binary operators can hash");
		if (leftSortName || rightSortName)
			elog(ERROR, "OperatorCreate: only binary operators can have sort links");
	}

	/* ----------------
	 *	Use OperatorDef() to define the specified operator and
	 *	also create shells for the operator's associated operators
	 *	if they don't already exist.
	 * ----------------
	 */
	OperatorDef(operatorName,
				leftTypeName,
				rightTypeName,
				procedureName,
				precedence,
				isLeftAssociative,
				commutatorName,
				negatorName,
				restrictionName,
				joinName,
				canHash,
				leftSortName,
				rightSortName);
}
