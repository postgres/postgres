/*-------------------------------------------------------------------------
 *
 * dllist.c
 *	  this is a simple doubly linked list implementation
 *	  the elements of the lists are void*
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/lib/dllist.c,v 1.36 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/dllist.h"


Dllist *
DLNewList(void)
{
	Dllist	   *l;

	l = (Dllist *) palloc(sizeof(Dllist));

	l->dll_head = NULL;
	l->dll_tail = NULL;

	return l;
}

void
DLInitList(Dllist *list)
{
	list->dll_head = NULL;
	list->dll_tail = NULL;
}

/*
 * free up a list and all the nodes in it --- but *not* whatever the nodes
 * might point to!
 */
void
DLFreeList(Dllist *list)
{
	Dlelem	   *curr;

	while ((curr = DLRemHead(list)) != NULL)
		pfree(curr);

	pfree(list);
}

Dlelem *
DLNewElem(void *val)
{
	Dlelem	   *e;

	e = (Dlelem *) palloc(sizeof(Dlelem));

	e->dle_next = NULL;
	e->dle_prev = NULL;
	e->dle_val = val;
	e->dle_list = NULL;
	return e;
}

void
DLInitElem(Dlelem *e, void *val)
{
	e->dle_next = NULL;
	e->dle_prev = NULL;
	e->dle_val = val;
	e->dle_list = NULL;
}

void
DLFreeElem(Dlelem *e)
{
	pfree(e);
}

void
DLRemove(Dlelem *e)
{
	Dllist	   *l = e->dle_list;

	if (e->dle_prev)
		e->dle_prev->dle_next = e->dle_next;
	else
	{
		/* must be the head element */
		Assert(e == l->dll_head);
		l->dll_head = e->dle_next;
	}
	if (e->dle_next)
		e->dle_next->dle_prev = e->dle_prev;
	else
	{
		/* must be the tail element */
		Assert(e == l->dll_tail);
		l->dll_tail = e->dle_prev;
	}

	e->dle_next = NULL;
	e->dle_prev = NULL;
	e->dle_list = NULL;
}

void
DLAddHead(Dllist *l, Dlelem *e)
{
	e->dle_list = l;

	if (l->dll_head)
		l->dll_head->dle_prev = e;
	e->dle_next = l->dll_head;
	e->dle_prev = NULL;
	l->dll_head = e;

	if (l->dll_tail == NULL)	/* if this is first element added */
		l->dll_tail = e;
}

void
DLAddTail(Dllist *l, Dlelem *e)
{
	e->dle_list = l;

	if (l->dll_tail)
		l->dll_tail->dle_next = e;
	e->dle_prev = l->dll_tail;
	e->dle_next = NULL;
	l->dll_tail = e;

	if (l->dll_head == NULL)	/* if this is first element added */
		l->dll_head = e;
}

Dlelem *
DLRemHead(Dllist *l)
{
	/* remove and return the head */
	Dlelem	   *result = l->dll_head;

	if (result == NULL)
		return result;

	if (result->dle_next)
		result->dle_next->dle_prev = NULL;

	l->dll_head = result->dle_next;

	if (result == l->dll_tail)	/* if the head is also the tail */
		l->dll_tail = NULL;

	result->dle_next = NULL;
	result->dle_list = NULL;

	return result;
}

Dlelem *
DLRemTail(Dllist *l)
{
	/* remove and return the tail */
	Dlelem	   *result = l->dll_tail;

	if (result == NULL)
		return result;

	if (result->dle_prev)
		result->dle_prev->dle_next = NULL;

	l->dll_tail = result->dle_prev;

	if (result == l->dll_head)	/* if the tail is also the head */
		l->dll_head = NULL;

	result->dle_prev = NULL;
	result->dle_list = NULL;

	return result;
}

/* Same as DLRemove followed by DLAddHead, but faster */
void
DLMoveToFront(Dlelem *e)
{
	Dllist	   *l = e->dle_list;

	if (l->dll_head == e)
		return;					/* Fast path if already at front */

	Assert(e->dle_prev != NULL);	/* since it's not the head */
	e->dle_prev->dle_next = e->dle_next;

	if (e->dle_next)
		e->dle_next->dle_prev = e->dle_prev;
	else
	{
		/* must be the tail element */
		Assert(e == l->dll_tail);
		l->dll_tail = e->dle_prev;
	}

	l->dll_head->dle_prev = e;
	e->dle_next = l->dll_head;
	e->dle_prev = NULL;
	l->dll_head = e;
	/* We need not check dll_tail, since there must have been > 1 entry */
}
