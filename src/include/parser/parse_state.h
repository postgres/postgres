/*-------------------------------------------------------------------------
 *
 * parse_state.h--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_state.h,v 1.1 1996/08/28 07:23:56 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARSE_STATE_H
#define PARSE_STATE_H

/* state information used during parse analysis */
typedef struct ParseState {
    int 	p_last_resno; 
    List 	*p_target_resnos;
    Relation 	parser_current_rel;
    List 	*p_rtable;
    int 	p_query_is_rule;
    int		p_numAgg;
    List	*p_aggs;
} ParseState;


#endif /*PARSE_QUERY_H*/
