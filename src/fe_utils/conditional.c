/*-------------------------------------------------------------------------
 * A stack of automaton states to handle nested conditionals.
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/fe_utils/conditional.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "fe_utils/conditional.h"

/*
 * create stack
 */
ConditionalStack
conditional_stack_create(void)
{
	ConditionalStack cstack = pg_malloc(sizeof(ConditionalStackData));

	cstack->head = NULL;
	return cstack;
}

/*
 * Destroy all the elements from the stack. The stack itself is not freed.
 */
void
conditional_stack_reset(ConditionalStack cstack)
{
	if (!cstack)
		return;					/* nothing to do here */

	while (conditional_stack_pop(cstack))
		continue;
}

/*
 * destroy stack
 */
void
conditional_stack_destroy(ConditionalStack cstack)
{
	conditional_stack_reset(cstack);
	free(cstack);
}

/*
 * Create a new conditional branch.
 */
void
conditional_stack_push(ConditionalStack cstack, ifState new_state)
{
	IfStackElem *p = (IfStackElem *) pg_malloc(sizeof(IfStackElem));

	p->if_state = new_state;
	p->query_len = -1;
	p->paren_depth = -1;
	p->lex_state = NULL;
	p->next = cstack->head;
	cstack->head = p;
}

/*
 * Destroy the topmost conditional branch.
 * Returns false if there was no branch to end.
 */
bool
conditional_stack_pop(ConditionalStack cstack)
{
	IfStackElem *p = cstack->head;

	if (!p)
		return false;
	cstack->head = cstack->head->next;
	if (p->lex_state)
		free(p->lex_state);
	free(p);
	return true;
}

/*
 * Returns current stack depth, for debugging purposes.
 */
int
conditional_stack_depth(ConditionalStack cstack)
{
	if (cstack == NULL)
		return -1;
	else
	{
		IfStackElem *p = cstack->head;
		int			depth = 0;

		while (p != NULL)
		{
			depth++;
			p = p->next;
		}
		return depth;
	}
}

/*
 * Fetch the current state of the top of the stack.
 */
ifState
conditional_stack_peek(ConditionalStack cstack)
{
	if (conditional_stack_empty(cstack))
		return IFSTATE_NONE;
	return cstack->head->if_state;
}

/*
 * Change the state of the topmost branch.
 * Returns false if there was no branch state to set.
 */
bool
conditional_stack_poke(ConditionalStack cstack, ifState new_state)
{
	if (conditional_stack_empty(cstack))
		return false;
	cstack->head->if_state = new_state;
	return true;
}

/*
 * True if there are no active \if-blocks.
 */
bool
conditional_stack_empty(ConditionalStack cstack)
{
	return cstack->head == NULL;
}

/*
 * True if we should execute commands normally; that is, the current
 * conditional branch is active, or there is no open \if block.
 */
bool
conditional_active(ConditionalStack cstack)
{
	ifState		s = conditional_stack_peek(cstack);

	return s == IFSTATE_NONE || s == IFSTATE_TRUE || s == IFSTATE_ELSE_TRUE;
}

/*
 * Save current query buffer length in topmost stack entry.
 */
void
conditional_stack_set_query_len(ConditionalStack cstack, int len)
{
	Assert(!conditional_stack_empty(cstack));
	cstack->head->query_len = len;
}

/*
 * Fetch last-recorded query buffer length from topmost stack entry.
 * Will return -1 if no stack or it was never saved.
 */
int
conditional_stack_get_query_len(ConditionalStack cstack)
{
	if (conditional_stack_empty(cstack))
		return -1;
	return cstack->head->query_len;
}

/*
 * Save current lexer state in topmost stack entry.
 *
 * The lexer state is presumed to be a single pg_malloc'd chunk.
 * It will be freed automatically when the stack entry is popped.
 */
void
conditional_stack_set_lex_state(ConditionalStack cstack,
								struct PsqlScanStateSave *lex_state)
{
	Assert(!conditional_stack_empty(cstack));
	if (cstack->head->lex_state)	/* free old state, if any */
		free(cstack->head->lex_state);
	cstack->head->lex_state = lex_state;
}

/*
 * Fetch last-recorded lexer state from topmost stack entry.
 * Will return NULL if no stack or it was never saved.
 */
struct PsqlScanStateSave *
conditional_stack_get_lex_state(ConditionalStack cstack)
{
	if (conditional_stack_empty(cstack))
		return NULL;
	return cstack->head->lex_state;
}

/*
 * Save current parenthesis nesting depth in topmost stack entry.
 *
 * (These functions are obsolete, and kept around only to avoid API/ABI
 * breakage in the back branches.)
 */
void
conditional_stack_set_paren_depth(ConditionalStack cstack, int depth)
{
	Assert(!conditional_stack_empty(cstack));
	cstack->head->paren_depth = depth;
}

/*
 * Fetch last-recorded parenthesis nesting depth from topmost stack entry.
 * Will return -1 if no stack or it was never saved.
 */
int
conditional_stack_get_paren_depth(ConditionalStack cstack)
{
	if (conditional_stack_empty(cstack))
		return -1;
	return cstack->head->paren_depth;
}
