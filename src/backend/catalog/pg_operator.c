/*-------------------------------------------------------------------------
 *
 * pg_operator.c--
 *	  routines to support manipulation of the pg_operator relation
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/pg_operator.c,v 1.30 1998/11/27 19:51:50 vadim Exp $
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
							Oid rightObjectId);
static Oid OperatorGet(char *operatorName,
			char *leftTypeName,
			char *rightTypeName);

static Oid OperatorShellMakeWithOpenRelation(Relation pg_operator_desc,
								  char *operatorName,
								  Oid leftObjectId,
								  Oid rightObjectId);
static Oid OperatorShellMake(char *operatorName,
				  char *leftTypeName,
				  char *rightTypeName);

static void OperatorDef(char *operatorName,
			int definedOK,
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
 *	  leftObjectId		-- left oid of operator to fetch
 *	  rightObjectId		-- right oid of operator to fetch
 */
static Oid
OperatorGetWithOpenRelation(Relation pg_operator_desc,
							const char *operatorName,
							Oid leftObjectId,
							Oid rightObjectId)
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
	operatorObjectId = HeapTupleIsValid(tup) ? tup->t_data->t_oid : InvalidOid;

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
			char *rightTypeName)
{
	Relation	pg_operator_desc;

	Oid			operatorObjectId;
	Oid			leftObjectId = InvalidOid;
	Oid			rightObjectId = InvalidOid;
	bool		leftDefined = false;
	bool		rightDefined = false;

	/* ----------------
	 *	look up the operator types.
	 *
	 *	Note: types must be defined before operators
	 * ----------------
	 */
	if (leftTypeName)
	{
		leftObjectId = TypeGet(leftTypeName, &leftDefined);

		if (!OidIsValid(leftObjectId) || !leftDefined)
			elog(ERROR, "OperatorGet: left type '%s' nonexistent", leftTypeName);
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
		elog(ERROR, "OperatorGet: no argument types??");

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
												   rightObjectId);

	/* ----------------
	 *	close the relation and return the operator oid.
	 * ----------------
	 */
	heap_close(pg_operator_desc);

	return
		operatorObjectId;
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
	 *	initialize *values with the type name and
	 * ----------------
	 */
	i = 0;
	namestrcpy(&oname, operatorName);
	values[i++] = NameGetDatum(&oname);
	values[i++] = Int32GetDatum(GetUserId());
	values[i++] = (Datum) (uint16) 0;

	values[i++] = (Datum) 'b';	/* fill oprkind with a bogus value */

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

	return
		operatorObjectId;
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
	operatorObjectId =
		OperatorShellMakeWithOpenRelation(pg_operator_desc,
										  operatorName,
										  leftObjectId,
										  rightObjectId);
	/* ----------------
	 *	close the operator relation and return the oid.
	 * ----------------
	 */
	heap_close(pg_operator_desc);

	return
		operatorObjectId;
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
 *	  if so issue error if not definedOk, this is a duplicate
 *	  but if definedOk, save the Oid -- filling in a shell
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
 *						name as the main operator, set the ObjectId=0.
 *						Later OperatorCreate will make another call
 *						to OperatorDef which will cause this field
 *						to be filled in (because even though the names
 *						will be switched, they are the same name and
 *						at this point this ObjectId will then be defined)
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
 *		definedOK;				-- operator can already have an oid?
 *		leftTypeName;			-- X left type name
 *		rightTypeName;			-- X right type name
 *		procedureName;			-- procedure oid for operator code
 *		precedence;				-- operator precedence
 *		isLeftAssociative;		-- operator is left associative?
 *		commutatorName;			-- X commutator operator name
 *		negatorName;			-- X negator operator name
 *		restrictionName;		-- X restriction sel. procedure name
 *		joinName;				-- X join sel. procedure name
 *		canHash;				-- possible hash operator?
 *		leftSortName;			-- X left sort operator
 *		rightSortName;			-- X right sort operator
 */
static void
OperatorDef(char *operatorName,
			int definedOK,
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
	Oid			other_oid = 0;
	Oid			operatorObjectId;
	Oid			leftTypeId = InvalidOid;
	Oid			rightTypeId = InvalidOid;
	Oid			commutatorId = InvalidOid;
	Oid			negatorId = InvalidOid;
	bool		leftDefined = false;
	bool		rightDefined = false;
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
								   rightTypeName);

	if (OidIsValid(operatorObjectId) && !definedOK)
		elog(ERROR, "OperatorDef: operator \"%s\" already defined",
			 operatorName);

	if (leftTypeName)
		leftTypeId = TypeGet(leftTypeName, &leftDefined);

	if (rightTypeName)
		rightTypeId = TypeGet(rightTypeName, &rightDefined);

	if (!((OidIsValid(leftTypeId && leftDefined)) ||
		  (OidIsValid(rightTypeId && rightDefined))))
		elog(ERROR, "OperatorGet: no argument types??");

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
	values[Anum_pg_operator_oprresult - 1] =
		ObjectIdGetDatum(((Form_pg_proc)
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

	++i;						/* Skip "prorettype", this was done above */

	/*
	 * Set up the other operators.	If they do not currently exist, set up
	 * shells in order to get ObjectId's and call OperatorDef again later
	 * to fill in the shells.
	 */
	name[0] = commutatorName;
	name[1] = negatorName;
	name[2] = leftSortName;
	name[3] = rightSortName;

	for (j = 0; j < 4; ++j)
	{
		if (name[j])
		{

			/* for the commutator, switch order of arguments */
			if (j == 0)
			{
				other_oid = OperatorGet(name[j], rightTypeName, leftTypeName);
				commutatorId = other_oid;
			}
			else
			{
				other_oid = OperatorGet(name[j], leftTypeName, rightTypeName);
				if (j == 1)
					negatorId = other_oid;
			}

			if (OidIsValid(other_oid))	/* already in catalogs */
				values[i++] = ObjectIdGetDatum(other_oid);
			else if (strcmp(operatorName, name[j]) != 0)
			{
				/* not in catalogs, different from operator */

				/* for the commutator, switch order of arguments */
				if (j == 0)
				{
					other_oid = OperatorShellMake(name[j],
												  rightTypeName,
												  leftTypeName);
				}
				else
				{
					other_oid = OperatorShellMake(name[j],
												  leftTypeName,
												  rightTypeName);
				}

				if (!OidIsValid(other_oid))
					elog(ERROR,
						 "OperatorDef: can't create operator '%s'",
						 name[j]);
				values[i++] = ObjectIdGetDatum(other_oid);

			}
			else
/* not in catalogs, same as operator ??? */
				values[i++] = ObjectIdGetDatum(InvalidOid);

		}
		else
/* new operator is optional */
			values[i++] = ObjectIdGetDatum(InvalidOid);
	}

	/* last three fields were filled in first */

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
			heap_replace(pg_operator_desc, &tup->t_self, tup);
			setheapoverride(false);
		}
		else
			elog(ERROR, "OperatorDef: no operator %d", other_oid);

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
	 * It's possible that we're creating a skeleton operator here for the
	 * commute or negate attributes of a real operator.  If we are, then
	 * we're done.  If not, we may need to update the negator and
	 * commutator for this attribute.  The reason for this is that the
	 * user may want to create two operators (say < and >=).  When he
	 * defines <, if he uses >= as the negator or commutator, he won't be
	 * able to insert it later, since (for some reason) define operator
	 * defines it for him.	So what he does is to define > without a
	 * negator or commutator.  Then he defines >= with < as the negator
	 * and commutator.	As a side effect, this will update the > tuple if
	 * it has no commutator or negator defined.
	 *
	 * Alstublieft, Tom Vijlbrief.
	 */
	if (!definedOK)
		OperatorUpd(operatorObjectId, commutatorId, negatorId);
}

/* ----------------------------------------------------------------
 * OperatorUpd
 *
 *	For a given operator, look up its negator and commutator operators.
 *	If they are defined, but their negator and commutator operators
 *	(respectively) are not, then use the new operator for neg and comm.
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

	/* if the commutator and negator are the same operator, do one update */
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
					values[Anum_pg_operator_oprnegate - 1] =
						ObjectIdGetDatum(baseId);
					replaces[Anum_pg_operator_oprnegate - 1] = 'r';
				}

				if (!OidIsValid(t->oprcom))
				{
					values[Anum_pg_operator_oprcom - 1] =
						ObjectIdGetDatum(baseId);
					replaces[Anum_pg_operator_oprcom - 1] = 'r';
				}

				tup = heap_modifytuple(tup,
									   pg_operator_desc,
									   values,
									   nulls,
									   replaces);

				setheapoverride(true);
				heap_replace(pg_operator_desc, &tup->t_self, tup);
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
		heap_replace(pg_operator_desc, &tup->t_self, tup);
		setheapoverride(false);

		values[Anum_pg_operator_oprcom - 1] = (Datum) NULL;
		replaces[Anum_pg_operator_oprcom - 1] = ' ';
	}

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
		heap_replace(pg_operator_desc, &tup->t_self, tup);
		setheapoverride(false);
	}

	heap_endscan(pg_operator_scan);

	heap_close(pg_operator_desc);
}


/* ----------------------------------------------------------------
 * OperatorCreate
 *
 * Algorithm:
 *
 *	Since the commutator, negator, leftsortoperator, and rightsortoperator
 *	can be defined implicitly through OperatorCreate, must check before
 *	the main operator is added to see if they already exist.  If they
 *	do not already exist, OperatorDef makes a "shell" for each undefined
 *	one, and then OperatorCreate must call OperatorDef again to fill in
 *	each shell.  All this is necessary in order to get the right ObjectId's
 *	filled into the right fields.
 *
 *	The "definedOk" flag indicates that OperatorDef can be called on
 *	the operator even though it already has an entry in the PG_OPERATOR
 *	relation.  This allows shells to be filled in.	The user cannot
 *	forward declare operators, this is strictly an internal capability.
 *
 *	When the shells are filled in by subsequent calls to OperatorDef,
 *	all the fields are the same as the definition of the original operator
 *	except that the target operator name and the original operatorName
 *	are switched.  In the case of commutator and negator, special flags
 *	are set to indicate their status, telling the executor(?) that
 *	the operands are to be switched, or the outcome of the procedure
 *	negated.
 *
 * ************************* NOTE NOTE NOTE ******************************
 *
 *	If the execution of this utility is interrupted, the pg_operator
 *	catalog may be left in an inconsistent state.  Similarly, if
 *	something is removed from the pg_operator, pg_type, or pg_procedure
 *	catalog while this is executing, the results may be inconsistent.
 * ----------------------------------------------------------------
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
 *		joinName;				-- X join sel. procedure name
 *		canHash;				-- operator hashes
 *		leftSortName;			-- X left sort operator
 *		rightSortName;			-- X right sort operator
 *
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
	Oid			commObjectId,
				negObjectId;
	Oid			leftSortObjectId,
				rightSortObjectId;
	int			definedOK;

	if (!leftTypeName && !rightTypeName)
		elog(ERROR, "OperatorCreate : at least one of leftarg or rightarg must be defined");

	/* ----------------
	 *	get the oid's of the operator's associated operators, if possible.
	 * ----------------
	 */
	if (commutatorName)
		commObjectId = OperatorGet(commutatorName,		/* commute type order */
								   rightTypeName,
								   leftTypeName);
	else
		commObjectId = 0;

	if (negatorName)
		negObjectId = OperatorGet(negatorName,
								  leftTypeName,
								  rightTypeName);
	else
		negObjectId = 0;

	if (leftSortName)
		leftSortObjectId = OperatorGet(leftSortName,
									   leftTypeName,
									   rightTypeName);
	else
		leftSortObjectId = 0;

	if (rightSortName)
		rightSortObjectId = OperatorGet(rightSortName,
										rightTypeName,
										leftTypeName);
	else
		rightSortObjectId = 0;

	/* ----------------
	 *	Use OperatorDef() to define the specified operator and
	 *	also create shells for the operator's associated operators
	 *	if they don't already exist.
	 *
	 *	This operator should not be defined yet.
	 * ----------------
	 */
	definedOK = 0;

	OperatorDef(operatorName,
				definedOK,
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

	/* ----------------
	 *	Now fill in information in the operator's associated
	 *	operators.
	 *
	 *	These operators should be defined or have shells defined.
	 * ----------------
	 */
	definedOK = 1;

	if (!OidIsValid(commObjectId) && commutatorName)
		OperatorDef(commutatorName,
					definedOK,
					leftTypeName,		/* should eventually */
					rightTypeName,		/* commute order */
					procedureName,
					precedence,
					isLeftAssociative,
					operatorName,		/* commutator */
					negatorName,
					restrictionName,
					joinName,
					canHash,
					rightSortName,
					leftSortName);

	if (negatorName && !OidIsValid(negObjectId))
		OperatorDef(negatorName,
					definedOK,
					leftTypeName,
					rightTypeName,
					procedureName,
					precedence,
					isLeftAssociative,
					commutatorName,
					operatorName,		/* negator */
					restrictionName,
					joinName,
					canHash,
					leftSortName,
					rightSortName);

	if (leftSortName && !OidIsValid(leftSortObjectId))
		OperatorDef(leftSortName,
					definedOK,
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
					operatorName,		/* left sort */
					rightSortName);

	if (rightSortName && !OidIsValid(rightSortObjectId))
		OperatorDef(rightSortName,
					definedOK,
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
					operatorName);		/* right sort */
}
