/*-------------------------------------------------------------------------
 *
 * proclist.h
 *		operations on doubly-linked lists of pgprocnos
 *
 * The interface is similar to dlist from ilist.h, but uses pgprocno instead
 * of pointers.  This allows proclist_head to be mapped at different addresses
 * in different backends.
 *
 * See proclist_types.h for the structs that these functions operate on.  They
 * are separated to break a header dependency cycle with proc.h.
 *
 * Portions Copyright (c) 2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist.h
 *-------------------------------------------------------------------------
 */
#ifndef PROCLIST_H
#define PROCLIST_H

#include "storage/proc.h"
#include "storage/proclist_types.h"

/*
 * Initialize a proclist.
 */
static inline void
proclist_init(proclist_head *list)
{
	list->head = list->tail = INVALID_PGPROCNO;
}

/*
 * Is the list empty?
 */
static inline bool
proclist_is_empty(proclist_head *list)
{
	return list->head == INVALID_PGPROCNO;
}

/*
 * Get a pointer to a proclist_node inside a given PGPROC, given a procno and
 * an offset.
 */
static inline proclist_node *
proclist_node_get(int procno, size_t node_offset)
{
	char	   *entry = (char *) GetPGProcByNumber(procno);

	return (proclist_node *) (entry + node_offset);
}

/*
 * Insert a node at the beginning of a list.
 */
static inline void
proclist_push_head_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	if (list->head == INVALID_PGPROCNO)
	{
		Assert(list->tail == INVALID_PGPROCNO);
		node->next = node->prev = INVALID_PGPROCNO;
		list->head = list->tail = procno;
	}
	else
	{
		Assert(list->tail != INVALID_PGPROCNO);
		node->next = list->head;
		proclist_node_get(node->next, node_offset)->prev = procno;
		node->prev = INVALID_PGPROCNO;
		list->head = procno;
	}
}

/*
 * Insert a node a the end of a list.
 */
static inline void
proclist_push_tail_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	if (list->tail == INVALID_PGPROCNO)
	{
		Assert(list->head == INVALID_PGPROCNO);
		node->next = node->prev = INVALID_PGPROCNO;
		list->head = list->tail = procno;
	}
	else
	{
		Assert(list->head != INVALID_PGPROCNO);
		node->prev = list->tail;
		proclist_node_get(node->prev, node_offset)->next = procno;
		node->next = INVALID_PGPROCNO;
		list->tail = procno;
	}
}

/*
 * Delete a node.  The node must be in the list.
 */
static inline void
proclist_delete_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	if (node->prev == INVALID_PGPROCNO)
		list->head = node->next;
	else
		proclist_node_get(node->prev, node_offset)->next = node->next;

	if (node->next == INVALID_PGPROCNO)
		list->tail = node->prev;
	else
		proclist_node_get(node->next, node_offset)->prev = node->prev;
}

/*
 * Helper macros to avoid repetition of offsetof(PGPROC, <member>).
 * 'link_member' is the name of a proclist_node member in PGPROC.
 */
#define proclist_delete(list, procno, link_member) \
	proclist_delete_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_push_head(list, procno, link_member) \
	proclist_push_head_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_push_tail(list, procno, link_member) \
	proclist_push_tail_offset((list), (procno), offsetof(PGPROC, link_member))

/*
 * Iterate through the list pointed at by 'lhead', storing the current
 * position in 'iter'.  'link_member' is the name of a proclist_node member in
 * PGPROC.  Access the current position with iter.cur.
 *
 * The only list modification allowed while iterating is deleting the current
 * node with proclist_delete(list, iter.cur, node_offset).
 */
#define proclist_foreach_modify(iter, lhead, link_member)					\
	for (AssertVariableIsOfTypeMacro(iter, proclist_mutable_iter),			\
		 AssertVariableIsOfTypeMacro(lhead, proclist_head *),				\
		 (iter).cur = (lhead)->head,										\
		 (iter).next = (iter).cur == INVALID_PGPROCNO ? INVALID_PGPROCNO :	\
			 proclist_node_get((iter).cur,									\
							   offsetof(PGPROC, link_member))->next;		\
		 (iter).cur != INVALID_PGPROCNO;									\
		 (iter).cur = (iter).next,											\
		 (iter).next = (iter).cur == INVALID_PGPROCNO ? INVALID_PGPROCNO :	\
			 proclist_node_get((iter).cur,									\
							   offsetof(PGPROC, link_member))->next)

#endif
