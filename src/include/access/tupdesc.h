/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/tupdesc.h,v 1.43 2004/04/01 21:28:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"


typedef struct attrDefault
{
	AttrNumber	adnum;
	char	   *adbin;			/* nodeToString representation of expr */
} AttrDefault;

typedef struct constrCheck
{
	char	   *ccname;
	char	   *ccbin;			/* nodeToString representation of expr */
} ConstrCheck;

/* This structure contains constraints of a tuple */
typedef struct tupleConstr
{
	AttrDefault *defval;		/* array */
	ConstrCheck *check;			/* array */
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;
} TupleConstr;

/*
 * This structure contains all information (i.e. from Classes
 * pg_attribute, pg_attrdef, pg_constraint) for the structure of a tuple.
 *
 * Note that only user attributes, not system attributes, are mentioned in
 * TupleDesc; with the exception that tdhasoid indicates if OID is present.
 *
 * If the tuple is known to correspond to a named rowtype (such as a table's
 * rowtype) then tdtypeid identifies that type and tdtypmod is -1.  Otherwise
 * tdtypeid is RECORDOID, and tdtypmod can be either -1 for a fully anonymous
 * row type, or a value >= 0 to allow the rowtype to be looked up in the
 * typcache.c type cache.
 */
typedef struct tupleDesc
{
	int			natts;			/* Number of attributes in the tuple */
	Form_pg_attribute *attrs;
	/* attrs[N] is a pointer to the description of Attribute Number N+1.  */
	TupleConstr *constr;
	Oid			tdtypeid;		/* composite type ID for tuple type */
	int32		tdtypmod;		/* typmod for tuple type */
	bool		tdhasoid;		/* Tuple has oid attribute in its header */
}	*TupleDesc;


extern TupleDesc CreateTemplateTupleDesc(int natts, bool hasoid);

extern TupleDesc CreateTupleDesc(int natts, bool hasoid,
				Form_pg_attribute *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);

extern void TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   const char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim);

extern TupleDesc BuildDescForRelation(List *schema);

#endif   /* TUPDESC_H */
