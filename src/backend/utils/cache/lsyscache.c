/*-------------------------------------------------------------------------
 *
 * lsyscache.c
 *	  Convenience routines for common queries in the system catalog cache.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/lsyscache.c,v 1.50 2001/01/24 19:43:15 momjian Exp $
 *
 * NOTES
 *	  Eventually, the index information should go through here, too.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*				---------- AMOP CACHES ----------						 */

/*
 * op_class
 *
 *		Return t iff operator 'opno' is in operator class 'opclass' for
 *		access method 'amopid'.
 */
bool
op_class(Oid opno, Oid opclass, Oid amopid)
{
	return SearchSysCacheExists(AMOPOPID,
								ObjectIdGetDatum(opclass),
								ObjectIdGetDatum(opno),
								ObjectIdGetDatum(amopid),
								0);
}

/*				---------- ATTRIBUTE CACHES ----------					 */

/*
 * get_attname
 *
 *		Given the relation id and the attribute number,
 *		return the "attname" field from the attribute relation.
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such operator.
 */
char *
get_attname(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		char   *result;

		result = pstrdup(NameStr(att_tup->attname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * get_attnum
 *
 *		Given the relation id and the attribute name,
 *		return the "attnum" field from the attribute relation.
 */
AttrNumber
get_attnum(Oid relid, char *attname)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNAME,
						ObjectIdGetDatum(relid),
						PointerGetDatum(attname),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		AttrNumber	result;

		result = att_tup->attnum;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}

/*
 * get_atttype
 *
 *		Given the relation OID and the attribute number with the relation,
 *		return the attribute type OID.
 */
Oid
get_atttype(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		Oid		result;

		result = att_tup->atttypid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/* This routine uses the attname instead of the attnum because it
 * replaces the routine find_atttype, which is called sometimes when
 * only the attname, not the attno, is available.
 */
bool
get_attisset(Oid relid, char *attname)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNAME,
						ObjectIdGetDatum(relid),
						PointerGetDatum(attname),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		bool	result;

		result = att_tup->attisset;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_atttypmod
 *
 *		Given the relation id and the attribute number,
 *		return the "atttypmod" field from the attribute relation.
 */
int32
get_atttypmod(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;

	tp = SearchSysCache(ATTNUM,
						ObjectIdGetDatum(relid),
						Int16GetDatum(attnum),
						0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_attribute att_tup = (Form_pg_attribute) GETSTRUCT(tp);
		int32	result;

		result = att_tup->atttypmod;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return -1;
}

/*
 * get_attdispersion
 *
 *	  Retrieve the dispersion statistic for an attribute,
 *	  or produce an estimate if no info is available.
 *
 * min_estimate is the minimum estimate to return if insufficient data
 * is available to produce a reliable value.  This value may vary
 * depending on context.  (For example, when deciding whether it is
 * safe to use a hashjoin, we want to be more conservative than when
 * estimating the number of tuples produced by an equijoin.)
 */
double
get_attdispersion(Oid relid, AttrNumber attnum, double min_estimate)
{
	HeapTuple	atp;
	Form_pg_attribute att_tup;
	double		dispersion;
	Oid			atttypid;
	int32		ntuples;

	atp = SearchSysCache(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum),
						 0, 0);
	if (!HeapTupleIsValid(atp))
	{
		/* this should not happen */
		elog(ERROR, "get_attdispersion: no attribute tuple %u %d",
			 relid, attnum);
		return min_estimate;
	}

	att_tup = (Form_pg_attribute) GETSTRUCT(atp);

	dispersion = att_tup->attdispersion;
	atttypid = att_tup->atttypid;

	ReleaseSysCache(atp);

	if (dispersion > 0.0)
		return dispersion;		/* we have a specific estimate from VACUUM */

	/*
	 * Special-case boolean columns: the dispersion of a boolean is highly
	 * unlikely to be anywhere near 1/numtuples, instead it's probably
	 * more like 0.5.
	 *
	 * Are there any other cases we should wire in special estimates for?
	 */
	if (atttypid == BOOLOID)
		return 0.5;

	/*
	 * Dispersion is either 0 (no data available) or -1 (dispersion is
	 * 1/numtuples).  Either way, we need the relation size.
	 */

	atp = SearchSysCache(RELOID,
						 ObjectIdGetDatum(relid),
						 0, 0, 0);
	if (!HeapTupleIsValid(atp))
	{
		/* this should not happen */
		elog(ERROR, "get_attdispersion: no relation tuple %u", relid);
		return min_estimate;
	}

	ntuples = ((Form_pg_class) GETSTRUCT(atp))->reltuples;

	ReleaseSysCache(atp);

	if (ntuples == 0)
		return min_estimate;	/* no data available */

	if (dispersion < 0.0)		/* VACUUM thinks there are no duplicates */
		return 1.0 / (double) ntuples;

	/*
	 * VACUUM ANALYZE does not compute dispersion for system attributes,
	 * but some of them can reasonably be assumed unique anyway.
	 */
	if (attnum == ObjectIdAttributeNumber ||
		attnum == SelfItemPointerAttributeNumber)
		return 1.0 / (double) ntuples;
	if (attnum == TableOidAttributeNumber)
		return 1.0;

	/*
	 * VACUUM ANALYZE has not been run for this table. Produce an estimate
	 * = 1/numtuples.  This may produce unreasonably small estimates for
	 * large tables, so limit the estimate to no less than min_estimate.
	 */
	dispersion = 1.0 / (double) ntuples;
	if (dispersion < min_estimate)
		dispersion = min_estimate;

	return dispersion;
}

/*				---------- INDEX CACHE ----------						 */

/*		watch this space...
 */

/*				---------- OPERATOR CACHE ----------					 */

/*
 * get_opcode
 *
 *		Returns the regproc id of the routine used to implement an
 *		operator given the operator oid.
 */
RegProcedure
get_opcode(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure	result;

		result = optup->oprcode;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_opname
 *	  returns the name of the operator with the given opno
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such operator.
 */
char *
get_opname(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		char   *result;

		result = pstrdup(NameStr(optup->oprname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * op_mergejoinable
 *
 *		Returns the left and right sort operators and types corresponding to a
 *		mergejoinable operator, or nil if the operator is not mergejoinable.
 */
bool
op_mergejoinable(Oid opno, Oid ltype, Oid rtype, Oid *leftOp, Oid *rightOp)
{
	HeapTuple	tp;
	bool		result = false;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

		if (optup->oprlsortop &&
			optup->oprrsortop &&
			optup->oprleft == ltype &&
			optup->oprright == rtype)
		{
			*leftOp = optup->oprlsortop;
			*rightOp = optup->oprrsortop;
			result = true;
		}
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * op_hashjoinable
 *
 * Returns the hash operator corresponding to a hashjoinable operator,
 * or InvalidOid if the operator is not hashjoinable.
 */
Oid
op_hashjoinable(Oid opno, Oid ltype, Oid rtype)
{
	HeapTuple	tp;
	Oid			result = InvalidOid;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);

		if (optup->oprcanhash &&
			optup->oprleft == ltype &&
			optup->oprright == rtype)
			result = opno;
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * op_iscachable
 *
 * Get the proiscachable flag for the operator's underlying function.
 */
bool
op_iscachable(Oid opno)
{
	RegProcedure	funcid = get_opcode(opno);

	if (funcid == (RegProcedure) InvalidOid)
		elog(ERROR, "Operator OID %u does not exist", opno);

	return func_iscachable((Oid) funcid);
}

/*
 * get_commutator
 *
 *		Returns the corresponding commutator of an operator.
 */
Oid
get_commutator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid		result;

		result = optup->oprcom;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_negator
 *
 *		Returns the corresponding negator of an operator.
 */
Oid
get_negator(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		Oid		result;

		result = optup->oprnegate;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * get_oprrest
 *
 *		Returns procedure id for computing selectivity of an operator.
 */
RegProcedure
get_oprrest(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure	result;

		result = optup->oprrest;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*
 * get_oprjoin
 *
 *		Returns procedure id for computing selectivity of a join.
 */
RegProcedure
get_oprjoin(Oid opno)
{
	HeapTuple	tp;

	tp = SearchSysCache(OPEROID,
						ObjectIdGetDatum(opno),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_operator optup = (Form_pg_operator) GETSTRUCT(tp);
		RegProcedure	result;

		result = optup->oprjoin;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return (RegProcedure) InvalidOid;
}

/*				---------- FUNCTION CACHE ----------					 */

/*
 * get_func_rettype
 *		Given procedure id, return the function's result type.
 */
Oid
get_func_rettype(Oid funcid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "Function OID %u does not exist", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->prorettype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * func_iscachable
 *		Given procedure id, return the function's proiscachable flag.
 */
bool
func_iscachable(Oid funcid)
{
	HeapTuple	tp;
	bool		result;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "Function OID %u does not exist", funcid);

	result = ((Form_pg_proc) GETSTRUCT(tp))->proiscachable;
	ReleaseSysCache(tp);
	return result;
}

/*				---------- RELATION CACHE ----------					 */

#ifdef NOT_USED
/*
 * get_relnatts
 *
 *		Returns the number of attributes for a given relation.
 */
int
get_relnatts(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		int		result;

		result = reltup->relnatts;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidAttrNumber;
}
#endif

/*
 * get_rel_name
 *
 *		Returns the name of a given relation.
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such operator.
 */
char *
get_rel_name(Oid relid)
{
	HeapTuple	tp;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);
		char   *result;

		result = pstrdup(NameStr(reltup->relname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*				---------- TYPE CACHE ----------						 */

/*
 * get_typlen
 *
 *		Given the type OID, return the length of the type.
 */
int16
get_typlen(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		int16	result;

		result = typtup->typlen;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 0;
}

/*
 * get_typbyval
 *
 *		Given the type OID, determine whether the type is returned by value or
 *		not.  Returns true if by value, false if by reference.
 */
bool
get_typbyval(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		bool	result;

		result = typtup->typbyval;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return false;
}

/*
 * get_typlenbyval
 *
 *		A two-fer: given the type OID, return both typlen and typbyval.
 *
 *		Since both pieces of info are needed to know how to copy a Datum,
 *		many places need both.  Might as well get them with one cache lookup
 *		instead of two.  Also, this routine raises an error instead of
 *		returning a bogus value when given a bad type OID.
 */
void
get_typlenbyval(Oid typid, int16 *typlen, bool *typbyval)
{
	HeapTuple	tp;
	Form_pg_type typtup;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	*typlen = typtup->typlen;
	*typbyval = typtup->typbyval;
	ReleaseSysCache(tp);
}

#ifdef NOT_USED
char
get_typalign(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char	result;

		result = typtup->typalign;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 'i';
}

#endif

char
get_typstorage(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char	result;

		result = typtup->typstorage;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return 'p';
}

/*
 * get_typdefault
 *
 *	  Given a type OID, return the typdefault field associated with that
 *	  type, or Datum(NULL) if there is no typdefault.  (This implies
 *	  that pass-by-value types can't have a default value that has
 *	  a representation of zero.  Not worth fixing now.)
 *	  The result points to palloc'd storage for non-pass-by-value types.
 */
Datum
get_typdefault(Oid typid)
{
	HeapTuple	typeTuple;
	Form_pg_type type;
	struct varlena *typDefault;
	bool		isNull;
	int32		dataSize;
	int32		typLen;
	bool		typByVal;
	Datum		returnValue;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(typid),
							   0, 0, 0);

	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "get_typdefault: failed to lookup type %u", typid);

	type = (Form_pg_type) GETSTRUCT(typeTuple);

	/*
	 * First, see if there is a non-null typdefault field (usually there
	 * isn't)
	 */
	typDefault = (struct varlena *)
		DatumGetPointer(SysCacheGetAttr(TYPEOID,
										typeTuple,
										Anum_pg_type_typdefault,
										&isNull));

	if (isNull)
	{
		ReleaseSysCache(typeTuple);
		return PointerGetDatum(NULL);
	}

	/*
	 * Otherwise, extract/copy the value.
	 */
	dataSize = VARSIZE(typDefault) - VARHDRSZ;
	typLen = type->typlen;
	typByVal = type->typbyval;

	if (typByVal)
	{
		if (dataSize == typLen)
			returnValue = fetch_att(VARDATA(typDefault), typByVal, typLen);
		else
			returnValue = PointerGetDatum(NULL);
	}
	else if (typLen < 0)
	{
		/* variable-size type */
		if (dataSize < 0)
			returnValue = PointerGetDatum(NULL);
		else
		{
			returnValue = PointerGetDatum(palloc(VARSIZE(typDefault)));
			memcpy((char *) DatumGetPointer(returnValue),
				   (char *) typDefault,
				   (int) VARSIZE(typDefault));
		}
	}
	else
	{
		/* fixed-size pass-by-ref type */
		if (dataSize != typLen)
			returnValue = PointerGetDatum(NULL);
		else
		{
			returnValue = PointerGetDatum(palloc(dataSize));
			memcpy((char *) DatumGetPointer(returnValue),
				   VARDATA(typDefault),
				   (int) dataSize);
		}
	}

	ReleaseSysCache(typeTuple);

	return returnValue;
}

/*
 * get_typtype
 *
 *		Given the type OID, find if it is a basic type, a named relation
 *		or the generic type 'relation'.
 *		It returns the null char if the cache lookup fails...
 */
#ifdef NOT_USED
char
get_typtype(Oid typid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(typid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char	result;

		result = typtup->typtype;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return '\0';
}

#endif
