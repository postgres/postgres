/*-------------------------------------------------------------------------
 *
 * publicationcmds.h
 *	  prototypes for publicationcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/publicationcmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PUBLICATIONCMDS_H
#define PUBLICATIONCMDS_H

#include "catalog/objectaddress.h"
#include "parser/parse_node.h"
#include "utils/inval.h"

/* Same as MAXNUMMESSAGES in sinvaladt.c */
#define MAX_RELCACHE_INVAL_MSGS 4096

extern ObjectAddress CreatePublication(ParseState *pstate, CreatePublicationStmt *stmt);
extern void AlterPublication(ParseState *pstate, AlterPublicationStmt *stmt);
extern void RemovePublicationById(Oid pubid);
extern void RemovePublicationRelById(Oid proid);
extern void RemovePublicationSchemaById(Oid psoid);

extern ObjectAddress AlterPublicationOwner(const char *name, Oid newOwnerId);
extern void AlterPublicationOwner_oid(Oid subid, Oid newOwnerId);
extern void InvalidatePublicationRels(List *relids);
extern bool pub_rf_contains_invalid_column(Oid pubid, Relation relation,
										   List *ancestors, bool pubviaroot);
extern bool pub_contains_invalid_column(Oid pubid, Relation relation,
										List *ancestors, bool pubviaroot,
										bool pubgencols,
										bool *invalid_column_list,
										bool *invalid_gen_col);

#endif							/* PUBLICATIONCMDS_H */
