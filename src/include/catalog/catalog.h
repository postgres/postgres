/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/catalog.h,v 1.49 2010/02/26 02:01:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "catalog/catversion.h"
#include "catalog/pg_class.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

#define OIDCHARS		10		/* max chars printed by %u */
#define TABLESPACE_VERSION_DIRECTORY	"PG_" PG_MAJORVERSION "_" \
									CppAsString2(CATALOG_VERSION_NO)

extern const char *forkNames[];
extern ForkNumber forkname_to_number(char *forkName);

extern char *relpath(RelFileNode rnode, ForkNumber forknum);
extern char *GetDatabasePath(Oid dbNode, Oid spcNode);

extern bool IsSystemRelation(Relation relation);
extern bool IsToastRelation(Relation relation);

extern bool IsSystemClass(Form_pg_class reltuple);
extern bool IsToastClass(Form_pg_class reltuple);

extern bool IsSystemNamespace(Oid namespaceId);
extern bool IsToastNamespace(Oid namespaceId);

extern bool IsReservedName(const char *name);

extern bool IsSharedRelation(Oid relationId);

extern Oid	GetNewOid(Relation relation);
extern Oid GetNewOidWithIndex(Relation relation, Oid indexId,
				   AttrNumber oidcolumn);
extern Oid	GetNewRelFileNode(Oid reltablespace, Relation pg_class);

#endif   /* CATALOG_H */
