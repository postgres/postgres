/*-------------------------------------------------------------------------
 * A stack of automaton states to handle nested conditionals.
 *
 * Copyright (c) 2000-2020, PostgreSQL Global Development Group
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
 * destroy stack
 */
void
conditional_stack_destroy(ConditionalStack cstack)
{
	while (conditional_stack_pop(cstack))
		continue;
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
 * Save current parenthesis nesting depth in topmost stack entry.
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
