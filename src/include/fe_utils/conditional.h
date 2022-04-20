/*-------------------------------------------------------------------------
 * A stack of automaton states to handle nested conditionals.
 *
 * This file describes a stack of automaton states which
 * allow a manage nested conditionals.
 *
 * It is used by:
 * - "psql" interpreter for handling \if ... \endif
 * - "pgbench" interpreter for handling \if ... \endif
 * - "pgbench" syntax checker to test for proper nesting
 *
 * The stack holds the state of enclosing conditionals (are we in
 * a true branch? in a false branch? have we already encountered
 * a true branch?) so that the interpreter knows whether to execute
 * code and whether to evaluate conditions.
 *
 * Copyright (c) 2000-2022, PostgreSQL Global Development Group
 *
 * src/include/fe_utils/conditional.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONDITIONAL_H
#define CONDITIONAL_H

/*
 * Possible states of a single level of \if block.
 */
typedef enum ifState
{
	IFSTATE_NONE = 0,			/* not currently in an \if block */
	IFSTATE_TRUE,				/* currently in an \if or \elif that is true
								 * and all parent branches (if any) are true */
	IFSTATE_FALSE,				/* currently in an \if or \elif that is false
								 * but no true branch has yet been seen, and
								 * all parent branches (if any) are true */
	IFSTATE_IGNORED,			/* currently in an \elif that follows a true
								 * branch, or the whole \if is a child of a
								 * false parent branch */
	IFSTATE_ELSE_TRUE,			/* currently in an \else that is true and all
								 * parent branches (if any) are true */
	IFSTATE_ELSE_FALSE			/* currently in an \else that is false or
								 * ignored */
} ifState;

/*
 * The state of nested \ifs is stored in a stack.
 *
 * query_len is used to determine what accumulated text to throw away at the
 * end of an inactive branch.  (We could, perhaps, teach the lexer to not add
 * stuff to the query buffer in the first place when inside an inactive branch;
 * but that would be very invasive.)  We also need to save and restore the
 * lexer's parenthesis nesting depth when throwing away text.  (We don't need
 * to save and restore any of its other state, such as comment nesting depth,
 * because a backslash command could never appear inside a comment or SQL
 * literal.)
 */
typedef struct IfStackElem
{
	ifState		if_state;		/* current state, see enum above */
	int			query_len;		/* length of query_buf at last branch start */
	int			paren_depth;	/* parenthesis depth at last branch start */
	struct IfStackElem *next;	/* next surrounding \if, if any */
} IfStackElem;

typedef struct ConditionalStackData
{
	IfStackElem *head;
}			ConditionalStackData;

typedef struct ConditionalStackData *ConditionalStack;


extern ConditionalStack conditional_stack_create(void);

extern void conditional_stack_reset(ConditionalStack cstack);

extern void conditional_stack_destroy(ConditionalStack cstack);

extern int	conditional_stack_depth(ConditionalStack cstack);

extern void conditional_stack_push(ConditionalStack cstack, ifState new_state);

extern bool conditional_stack_pop(ConditionalStack cstack);

extern ifState conditional_stack_peek(ConditionalStack cstack);

extern bool conditional_stack_poke(ConditionalStack cstack, ifState new_state);

extern bool conditional_stack_empty(ConditionalStack cstack);

extern bool conditional_active(ConditionalStack cstack);

extern void conditional_stack_set_query_len(ConditionalStack cstack, int len);

extern int	conditional_stack_get_query_len(ConditionalStack cstack);

extern void conditional_stack_set_paren_depth(ConditionalStack cstack, int depth);

extern int	conditional_stack_get_paren_depth(ConditionalStack cstack);

#endif							/* CONDITIONAL_H */
