/*-------------------------------------------------------------------------
 *
 * pg_inherits_fn.h
 *	  prototypes for functions in catalog/pg_inherits.c
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_inherits_fn.h,v 1.1 2009/05/12 03:11:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INHERITS_FN_H
#define PG_INHERITS_FN_H

#include "nodes/pg_list.h"
#include "storage/lock.h"

extern List *find_inheritance_children(Oid parentrelId, LOCKMODE lockmode);
extern List *find_all_inheritors(Oid parentrelId, LOCKMODE lockmode);
extern bool has_subclass(Oid relationId);
extern bool typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId);

#endif   /* PG_INHERITS_FN_H */
