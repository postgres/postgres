/*-------------------------------------------------------------------------
 *
 * parse_state.h--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_state.h,v 1.6 1997/09/07 04:59:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARSE_STATE_H
#define PARSE_STATE_H

#include <nodes/parsenodes.h>
#include <utils/rel.h>

/* state information used during parse analysis */
typedef struct ParseState
{
	int				p_last_resno;
	List		   *p_rtable;
	int				p_numAgg;
	List		   *p_aggs;
	bool			p_is_insert;
	List		   *p_insert_columns;
	bool			p_is_update;
	bool			p_is_rule;
	Relation		p_target_relation;
	RangeTblEntry  *p_target_rangetblentry;
}				ParseState;


#endif							/* PARSE_QUERY_H */
