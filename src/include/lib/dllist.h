/*-------------------------------------------------------------------------
 *
 * dllist.h
 *		simple doubly linked list primitives
 *		the elements of the list are void* so the lists can contain anything
 *		Dlelem can only be in one list at a time
 *
 *
 *	 Here's a small example of how to use Dllists:
 *
 *	 Dllist *lst;
 *	 Dlelem *elt;
 *	 void	*in_stuff;	  -- stuff to stick in the list
 *	 void	*out_stuff
 *
 *	 lst = DLNewList();				   -- make a new dllist
 *	 DLAddHead(lst, DLNewElem(in_stuff)); -- add a new element to the list
 *											 with in_stuff as the value
 *	  ...
 *	 elt = DLGetHead(lst);			   -- retrieve the head element
 *	 out_stuff = (void*)DLE_VAL(elt);  -- get the stuff out
 *	 DLRemove(elt);					   -- removes the element from its list
 *	 DLFreeElem(elt);				   -- free the element since we don't
 *										  use it anymore
 *
 *
 * It is also possible to use Dllist objects that are embedded in larger
 * structures instead of being separately malloc'd.  To do this, use
 * DLInitElem() to initialize a Dllist field within a larger object.
 * Don't forget to DLRemove() each field from its list (if any) before
 * freeing the larger object!
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/lib/dllist.h,v 1.30 2010/01/02 16:58:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef DLLIST_H
#define DLLIST_H

struct Dllist;
struct Dlelem;

typedef struct Dlelem
{
	struct Dlelem *dle_next;	/* next element */
	struct Dlelem *dle_prev;	/* previous element */
	void	   *dle_val;		/* value of the element */
	struct Dllist *dle_list;	/* what list this element is in */
} Dlelem;

typedef struct Dllist
{
	Dlelem	   *dll_head;
	Dlelem	   *dll_tail;
} Dllist;

extern Dllist *DLNewList(void); /* allocate and initialize a list header */
extern void DLInitList(Dllist *list);	/* init a header alloced by caller */
extern void DLFreeList(Dllist *list);	/* free up a list and all the nodes in
										 * it */
extern Dlelem *DLNewElem(void *val);
extern void DLInitElem(Dlelem *e, void *val);
extern void DLFreeElem(Dlelem *e);
extern void DLRemove(Dlelem *e);	/* removes node from list */
extern void DLAddHead(Dllist *list, Dlelem *node);
extern void DLAddTail(Dllist *list, Dlelem *node);
extern Dlelem *DLRemHead(Dllist *list); /* remove and return the head */
extern Dlelem *DLRemTail(Dllist *list);
extern void DLMoveToFront(Dlelem *e);	/* move node to front of its list */

/* These are macros for speed */
#define DLGetHead(list)  ((list)->dll_head)
#define DLGetTail(list)  ((list)->dll_tail)
#define DLGetSucc(elem)  ((elem)->dle_next)
#define DLGetPred(elem)  ((elem)->dle_prev)
#define DLGetListHdr(elem)	((elem)->dle_list)

#define DLE_VAL(elem)	((elem)->dle_val)

#endif   /* DLLIST_H */
