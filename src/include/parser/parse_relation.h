 /*-------------------------------------------------------------------------
 *
 * parse_query.h--
 *	  prototypes for parse_query.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_relation.h,v 1.2 1997/11/26 01:14:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_QUERY_H
#define PARSE_RANGE_H

#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/pg_list.h>
#include <nodes/primnodes.h>
#include <parser/parse_node.h>
#include <utils/rel.h>

extern RangeTblEntry *refnameRangeTableEntry(List *rtable, char *refname);
extern int refnameRangeTablePosn(List *rtable, char *refname);
extern RangeTblEntry *colnameRangeTableEntry(ParseState *pstate, char *colname);
extern RangeTblEntry *addRangeTableEntry(ParseState *pstate,
								   char *relname,
								   char *refname,
								   bool inh,
								   bool inFromCl);
extern List *expandAll(ParseState *pstate, char *relname, char *refname,
						int *this_resno);
extern int attnameAttNum(Relation rd, char *a);
extern bool attnameIsSet(Relation rd, char *name);
extern char *attnumAttName(Relation rd, int attrno);
extern int attnumAttNelems(Relation rd, int attid);
extern Oid attnameTypeId(Oid relid, char *attrname);
extern Oid attnumTypeId(Relation rd, int attid);
extern void handleTargetColname(ParseState *pstate, char **resname,
					char *refname, char *colname);
extern void checkTargetTypes(ParseState *pstate, char *target_colname,
				 char *refname, char *colname);

#endif							/* PARSE_RANGE_H */
