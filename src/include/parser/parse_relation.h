/*-------------------------------------------------------------------------
 *
 * parse_relation.h
 *	  prototypes for parse_relation.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_relation.h,v 1.13 1999/10/07 04:23:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_RELATION_H
#define PARSE_RELATION_H

#include "parser/parse_node.h"

extern RangeTblEntry *refnameRangeTableEntry(ParseState *pstate, char *refname);
extern int refnameRangeTablePosn(ParseState *pstate,
					  char *refname, int *sublevels_up);
extern RangeTblEntry *colnameRangeTableEntry(ParseState *pstate, char *colname);
extern RangeTblEntry *addRangeTableEntry(ParseState *pstate,
				   char *relname,
				   char *refname,
				   bool inh,
				   bool inFromCl,
				   bool inJoinSet);
extern List *expandAll(ParseState *pstate, char *relname, char *refname,
		  int *this_resno);
extern int	attnameAttNum(Relation rd, char *a);
extern bool attnameIsSet(Relation rd, char *name);
extern int	attnumAttNelems(Relation rd, int attid);
extern Oid	attnumTypeId(Relation rd, int attid);

#endif	 /* PARSE_RELATION_H */
