/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tupdesc.h,v 1.41 2003/08/11 23:04:50 tgl Exp $
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
 */
typedef struct tupleDesc
{
	int			natts;			/* Number of attributes in the tuple */
	Form_pg_attribute *attrs;
	/* attrs[N] is a pointer to the description of Attribute Number N+1.  */
	TupleConstr *constr;
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
				   int attdim,
				   bool attisset);

extern TupleDesc BuildDescForRelation(List *schema);

#endif   /* TUPDESC_H */
