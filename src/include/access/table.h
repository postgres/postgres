/*-------------------------------------------------------------------------
 *
 * table.h
 *	  Generic routines for table related code.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/table.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLE_H
#define TABLE_H

#include "nodes/primnodes.h"
#include "utils/relcache.h"
#include "storage/lockdefs.h"


extern Relation table_open(Oid relationId, LOCKMODE lockmode);
extern Relation table_openrv(const RangeVar *relation, LOCKMODE lockmode);
extern Relation table_openrv_extended(const RangeVar *relation,
									  LOCKMODE lockmode, bool missing_ok);
extern void table_close(Relation relation, LOCKMODE lockmode);

/*
 * heap_ used to be the prefix for these routines, and a lot of code will just
 * continue to work without adaptions after the introduction of pluggable
 * storage, therefore just map these names.
 */
#define heap_open(r, l)					table_open(r, l)
#define heap_openrv(r, l)				table_openrv(r, l)
#define heap_openrv_extended(r, l, m)	table_openrv_extended(r, l, m)
#define heap_close(r, l)				table_close(r, l)

#endif							/* TABLE_H */
