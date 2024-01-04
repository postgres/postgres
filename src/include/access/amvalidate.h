/*-------------------------------------------------------------------------
 *
 * amvalidate.h
 *	  Support routines for index access methods' amvalidate and
 *	  amadjustmembers functions.
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 * src/include/access/amvalidate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMVALIDATE_H
#define AMVALIDATE_H

#include "utils/catcache.h"


/* Struct returned (in a list) by identify_opfamily_groups() */
typedef struct OpFamilyOpFuncGroup
{
	Oid			lefttype;		/* amoplefttype/amproclefttype */
	Oid			righttype;		/* amoprighttype/amprocrighttype */
	uint64		operatorset;	/* bitmask of operators with these types */
	uint64		functionset;	/* bitmask of support funcs with these types */
} OpFamilyOpFuncGroup;


/* Functions in access/index/amvalidate.c */
extern List *identify_opfamily_groups(CatCList *oprlist, CatCList *proclist);
extern bool check_amproc_signature(Oid funcid, Oid restype, bool exact,
								   int minargs, int maxargs,...);
extern bool check_amoptsproc_signature(Oid funcid);
extern bool check_amop_signature(Oid opno, Oid restype,
								 Oid lefttype, Oid righttype);
extern Oid	opclass_for_family_datatype(Oid amoid, Oid opfamilyoid,
										Oid datatypeoid);
extern bool opfamily_can_sort_type(Oid opfamilyoid, Oid datatypeoid);

#endif							/* AMVALIDATE_H */
