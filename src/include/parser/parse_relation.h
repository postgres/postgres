 /*-------------------------------------------------------------------------
 *
 * parse_query.h--
 *	  prototypes for parse_query.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_relation.h,v 1.1 1997/11/25 22:07:02 momjian Exp $
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

RangeTblEntry *refnameRangeTableEntry(List *rtable, char *refname);

int refnameRangeTablePosn(List *rtable, char *refname);

RangeTblEntry *colnameRangeTableEntry(ParseState *pstate, char *colname);

RangeTblEntry *addRangeTableEntry(ParseState *pstate,
								   char *relname,
								   char *refname,
								   bool inh,
								   bool inFromCl);

List *expandAll(ParseState *pstate, char *relname, char *refname,
						int *this_resno);

int attnameAttNum(Relation rd, char *a);

bool attnameIsSet(Relation rd, char *name);

char *attnumAttName(Relation rd, int attrno);

int attnumAttNelems(Relation rd, int attid);

Oid attnameTypeId(Oid relid, char *attrname);

Oid attnumTypeId(Relation rd, int attid);

void handleTargetColname(ParseState *pstate, char **resname,
					char *refname, char *colname);

void checkTargetTypes(ParseState *pstate, char *target_colname,
				 char *refname, char *colname);

#endif							/* PARSE_RANGE_H */
