/*-------------------------------------------------------------------------
 *
 * parse_state.h--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_state.h,v 1.4 1996/10/30 02:02:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARSE_STATE_H
#define PARSE_STATE_H

/* state information used during parse analysis */
typedef struct ParseState {
    int 	p_last_resno;
    List 	*p_rtable;
    int		p_numAgg;
    List	*p_aggs;
    bool	p_is_insert;
    List	*p_insert_columns;
    bool	p_is_update;
    bool	p_is_rule;
    Relation	p_target_relation;
    RangeTblEntry *p_target_rangetblentry;
} ParseState;


#endif /*PARSE_QUERY_H*/
