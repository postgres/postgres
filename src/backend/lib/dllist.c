/*-------------------------------------------------------------------------
 *
 * dllist.c
 *	  this is a simple doubly linked list implementation
 *	  the elements of the lists are void*
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/lib/dllist.c,v 1.27 2003/08/04 02:39:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/* can be used in frontend or backend */
#ifdef FRONTEND
#include "postgres_fe.h"
/* No assert checks in frontend ... */
#define Assert(condition)
#else
#include "postgres.h"
#endif

#include "lib/dllist.h"


Dllist *
DLNewList(void)
{
	Dllist	   *l;

	l = (Dllist *) malloc(sizeof(Dllist));
	if (l == NULL)
	{
#ifdef FRONTEND
		fprintf(stderr, "memory exhausted in DLNewList\n");
		exit(1);
#else
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
#endif
	}
	l->dll_head = 0;
	l->dll_tail = 0;

	return l;
}

void
DLInitList(Dllist *list)
{
	list->dll_head = 0;
	list->dll_tail = 0;
}

/*
 * free up a list and all the nodes in it --- but *not* whatever the nodes
 * might point to!
 */
void
DLFreeList(Dllist *list)
{
	Dlelem	   *curr;

	while ((curr = DLRemHead(list)) != 0)
		free(curr);

	free(list);
}

Dlelem *
DLNewElem(void *val)
{
	Dlelem	   *e;

	e = (Dlelem *) malloc(sizeof(Dlelem));
	if (e == NULL)
	{
#ifdef FRONTEND
		fprintf(stderr, "memory exhausted in DLNewElem\n");
		exit(1);
#else
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
#endif
	}
	e->dle_next = 0;
	e->dle_prev = 0;
	e->dle_val = val;
	e->dle_list = 0;
	return e;
}

void
DLInitElem(Dlelem *e, void *val)
{
	e->dle_next = 0;
	e->dle_prev = 0;
	e->dle_val = val;
	e->dle_list = 0;
}

void
DLFreeElem(Dlelem *e)
{
	free(e);
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

	e->dle_next = 0;
	e->dle_prev = 0;
	e->dle_list = 0;
}

void
DLAddHead(Dllist *l, Dlelem *e)
{
	e->dle_list = l;

	if (l->dll_head)
		l->dll_head->dle_prev = e;
	e->dle_next = l->dll_head;
	e->dle_prev = 0;
	l->dll_head = e;

	if (l->dll_tail == 0)		/* if this is first element added */
		l->dll_tail = e;
}

void
DLAddTail(Dllist *l, Dlelem *e)
{
	e->dle_list = l;

	if (l->dll_tail)
		l->dll_tail->dle_next = e;
	e->dle_prev = l->dll_tail;
	e->dle_next = 0;
	l->dll_tail = e;

	if (l->dll_head == 0)		/* if this is first element added */
		l->dll_head = e;
}

Dlelem *
DLRemHead(Dllist *l)
{
	/* remove and return the head */
	Dlelem	   *result = l->dll_head;

	if (result == 0)
		return result;

	if (result->dle_next)
		result->dle_next->dle_prev = 0;

	l->dll_head = result->dle_next;

	if (result == l->dll_tail)	/* if the head is also the tail */
		l->dll_tail = 0;

	result->dle_next = 0;
	result->dle_list = 0;

	return result;
}

Dlelem *
DLRemTail(Dllist *l)
{
	/* remove and return the tail */
	Dlelem	   *result = l->dll_tail;

	if (result == 0)
		return result;

	if (result->dle_prev)
		result->dle_prev->dle_next = 0;

	l->dll_tail = result->dle_prev;

	if (result == l->dll_head)	/* if the tail is also the head */
		l->dll_head = 0;

	result->dle_prev = 0;
	result->dle_list = 0;

	return result;
}

/* Same as DLRemove followed by DLAddHead, but faster */
void
DLMoveToFront(Dlelem *e)
{
	Dllist	   *l = e->dle_list;

	if (l->dll_head == e)
		return;					/* Fast path if already at front */

	Assert(e->dle_prev != 0);	/* since it's not the head */
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
	e->dle_prev = 0;
	l->dll_head = e;
	/* We need not check dll_tail, since there must have been > 1 entry */
}
