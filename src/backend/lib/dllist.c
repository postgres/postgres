/*-------------------------------------------------------------------------
 *
 * dllist.c--
 *	  this is a simple doubly linked list implementation
 *	  replaces the old simplelists stuff
 *	  the elements of the lists are void*
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/lib/dllist.c,v 1.11 1998/09/01 03:22:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <lib/dllist.h>

Dllist *
DLNewList(void)
{
	Dllist	   *l;

	l = malloc(sizeof(Dllist));
	l->dll_head = 0;
	l->dll_tail = 0;

	return l;
}

 /* free up a list and all the nodes in it */
void
DLFreeList(Dllist *l)
{
	Dlelem	   *curr;

	while ((curr = DLRemHead(l)) != 0)
		free(curr);

	free(l);
}

Dlelem *
DLNewElem(void *val)
{
	Dlelem	   *e;

	e = malloc(sizeof(Dlelem));
	e->dle_next = 0;
	e->dle_prev = 0;
	e->dle_val = val;
	e->dle_list = 0;
	return e;
}

void
DLFreeElem(Dlelem *e)
{
	free(e);
}

Dlelem *
DLGetHead(Dllist *l)
{
	return l ? l->dll_head : 0;
}

/* get the value stored in the first element */
#ifdef NOT_USED
void *
DLGetHeadVal(Dllist *l)
{
	Dlelem	   *e = DLGetHead(l);

	return e ? e->dle_val : 0;
}

#endif

Dlelem *
DLGetTail(Dllist *l)
{
	return l ? l->dll_tail : 0;
}

/* get the value stored in the first element */
#ifdef NOT_USED
void *
DLGetTailVal(Dllist *l)
{
	Dlelem	   *e = DLGetTail(l);

	return e ? e->dle_val : 0;
}

#endif

Dlelem *
DLGetPred(Dlelem *e)			/* get predecessor */
{
	return e ? e->dle_prev : 0;
}

Dlelem *
DLGetSucc(Dlelem *e)			/* get successor */
{
	return e ? e->dle_next : 0;
}

void
DLRemove(Dlelem *e)
{
	Dllist	   *l;

	if (e->dle_prev)
		e->dle_prev->dle_next = e->dle_next;
	if (e->dle_next)
		e->dle_next->dle_prev = e->dle_prev;

	/* check to see if we're removing the head or tail */
	l = e->dle_list;
	if (e == l->dll_head)
		DLRemHead(l);
	if (e == l->dll_tail)
		DLRemTail(l);

}

void
DLAddHead(Dllist *l, Dlelem *e)
{
	e->dle_list = l;

	if (l->dll_head)
	{
		l->dll_head->dle_prev = e;
		e->dle_next = l->dll_head;
	}
	e->dle_prev = 0;
	l->dll_head = e;

	if (l->dll_tail == 0)		/* if this is first element added */
		l->dll_tail = l->dll_head;
}

void
DLAddTail(Dllist *l, Dlelem *e)
{
	e->dle_list = l;

	if (l->dll_tail)
	{
		l->dll_tail->dle_next = e;
		e->dle_prev = l->dll_tail;
	}
	e->dle_next = 0;
	l->dll_tail = e;

	if (l->dll_head == 0)		/* if this is first element added */
		l->dll_head = l->dll_tail;
}

Dlelem *
DLRemHead(Dllist *l)
{
	/* remove and return the head */
	Dlelem	   *result;

	if (l->dll_head == 0)
		return 0;

	result = l->dll_head;
	if (l->dll_head->dle_next)
		l->dll_head->dle_next->dle_prev = 0;

	l->dll_head = l->dll_head->dle_next;

	result->dle_next = 0;
	result->dle_list = 0;

	if (result == l->dll_tail)	/* if the head is also the tail */
		l->dll_tail = 0;

	return result;
}

Dlelem *
DLRemTail(Dllist *l)
{
	/* remove and return the tail */
	Dlelem	   *result;

	if (l->dll_tail == 0)
		return 0;

	result = l->dll_tail;
	if (l->dll_tail->dle_prev)
		l->dll_tail->dle_prev->dle_next = 0;
	l->dll_tail = l->dll_tail->dle_prev;

	result->dle_prev = 0;
	result->dle_list = 0;

	if (result == l->dll_head)	/* if the tail is also the head */
		l->dll_head = 0;

	return result;
}
