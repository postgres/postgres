/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/view.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VIEW_H
#define VIEW_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

extern ObjectAddress DefineView(ViewStmt *stmt, const char *queryString,
								int stmt_location, int stmt_len);

extern void StoreViewQuery(Oid viewOid, Query *viewParse, bool replace);

#endif							/* VIEW_H */
