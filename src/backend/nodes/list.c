/*-------------------------------------------------------------------------
 *
 * list.c
 *	  POSTGRES generic list package
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/list.c,v 1.54 2003/08/08 21:41:44 momjian Exp $
 *
 * NOTES
 *	  XXX a few of the following functions are duplicated to handle
 *		  List of pointers and List of integers separately. Some day,
 *		  someone should unify them.			- ay 11/2/94
 *	  This file needs cleanup.
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Oct, 1994		file creation
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/parsenodes.h"


/*
 *	makeInteger
 */
Value *
makeInteger(long i)
{
	Value	   *v = makeNode(Value);

	v->type = T_Integer;
	v->val.ival = i;
	return v;
}

/*
 *	makeFloat
 *
 * Caller is responsible for passing a palloc'd string.
 */
Value *
makeFloat(char *numericStr)
{
	Value	   *v = makeNode(Value);

	v->type = T_Float;
	v->val.str = numericStr;
	return v;
}

/*
 *	makeString
 *
 * Caller is responsible for passing a palloc'd string.
 */
Value *
makeString(char *str)
{
	Value	   *v = makeNode(Value);

	v->type = T_String;
	v->val.str = str;
	return v;
}


/*
 *	makeBitString
 *
 * Caller is responsible for passing a palloc'd string.
 */
Value *
makeBitString(char *str)
{
	Value	   *v = makeNode(Value);

	v->type = T_BitString;
	v->val.str = str;
	return v;
}


/*
 *	lcons
 *
 *	Add obj to the front of list, or make a new list if 'list' is NIL
 */
List *
lcons(void *obj, List *list)
{
	List	   *l = makeNode(List);

	lfirst(l) = obj;
	lnext(l) = list;
	return l;
}

/*
 *	lconsi
 *
 *	Same as lcons, but for integer data
 */
List *
lconsi(int datum, List *list)
{
	List	   *l = makeNode(List);

	lfirsti(l) = datum;
	lnext(l) = list;
	return l;
}

/*
 *	lconso
 *
 *	Same as lcons, but for Oid data
 */
List *
lconso(Oid datum, List *list)
{
	List	   *l = makeNode(List);

	lfirsto(l) = datum;
	lnext(l) = list;
	return l;
}

/*
 *	lappend
 *
 *	Add obj to the end of list, or make a new list if 'list' is NIL
 *
 * MORE EXPENSIVE THAN lcons
 */
List *
lappend(List *list, void *datum)
{
	return nconc(list, makeList1(datum));
}

/*
 *	lappendi
 *
 *	Same as lappend, but for integers
 */
List *
lappendi(List *list, int datum)
{
	return nconc(list, makeListi1(datum));
}

/*
 *	lappendo
 *
 *	Same as lappend, but for Oids
 */
List *
lappendo(List *list, Oid datum)
{
	return nconc(list, makeListo1(datum));
}

/*
 *	nconc
 *
 *	Concat l2 on to the end of l1
 *
 * NB: l1 is destructively changed!  Use nconc(listCopy(l1), l2)
 * if you need to make a merged list without touching the original lists.
 */
List *
nconc(List *l1, List *l2)
{
	List	   *temp;

	if (l1 == NIL)
		return l2;
	if (l2 == NIL)
		return l1;
	if (l1 == l2)
		elog(ERROR, "cannot nconc a list to itself");

	for (temp = l1; lnext(temp) != NIL; temp = lnext(temp))
		;

	lnext(temp) = l2;
	return l1;					/* list1 is now list1+list2  */
}

/*
 * FastAppend - append to a FastList.
 *
 * For long lists this is significantly faster than repeated lappend's,
 * since we avoid having to chase down the list again each time.
 */
void
FastAppend(FastList *fl, void *datum)
{
	List	   *cell = makeList1(datum);

	if (fl->tail)
	{
		lnext(fl->tail) = cell;
		fl->tail = cell;
	}
	else
	{
		/* First cell of list */
		Assert(fl->head == NIL);
		fl->head = fl->tail = cell;
	}
}

/*
 * FastAppendi - same for integers
 */
void
FastAppendi(FastList *fl, int datum)
{
	List	   *cell = makeListi1(datum);

	if (fl->tail)
	{
		lnext(fl->tail) = cell;
		fl->tail = cell;
	}
	else
	{
		/* First cell of list */
		Assert(fl->head == NIL);
		fl->head = fl->tail = cell;
	}
}

/*
 * FastAppendo - same for Oids
 */
void
FastAppendo(FastList *fl, Oid datum)
{
	List	   *cell = makeListo1(datum);

	if (fl->tail)
	{
		lnext(fl->tail) = cell;
		fl->tail = cell;
	}
	else
	{
		/* First cell of list */
		Assert(fl->head == NIL);
		fl->head = fl->tail = cell;
	}
}

/*
 * FastConc - nconc() for FastList building
 *
 * Note that the cells of the second argument are absorbed into the FastList.
 */
void
FastConc(FastList *fl, List *cells)
{
	if (cells == NIL)
		return;					/* nothing to do */
	if (fl->tail)
		lnext(fl->tail) = cells;
	else
	{
		/* First cell of list */
		Assert(fl->head == NIL);
		fl->head = cells;
	}
	while (lnext(cells) != NIL)
		cells = lnext(cells);
	fl->tail = cells;
}

/*
 * FastConcFast - nconc() for FastList building
 *
 * Note that the cells of the second argument are absorbed into the first.
 */
void
FastConcFast(FastList *fl, FastList *fl2)
{
	if (fl2->head == NIL)
		return;					/* nothing to do */
	if (fl->tail)
		lnext(fl->tail) = fl2->head;
	else
	{
		/* First cell of list */
		Assert(fl->head == NIL);
		fl->head = fl2->head;
	}
	fl->tail = fl2->tail;
}

/*
 *	nth
 *
 *	Get the n'th element of the list.  First element is 0th.
 */
void *
nth(int n, List *l)
{
	/* XXX assume list is long enough */
	while (n-- > 0)
		l = lnext(l);
	return lfirst(l);
}

/*
 *	length
 *
 *	Get the length of l
 */
int
length(List *l)
{
	int			i = 0;

	while (l != NIL)
	{
		l = lnext(l);
		i++;
	}
	return i;
}

/*
 *	llast
 *
 *	Get the last element of l ... error if empty list
 */
void *
llast(List *l)
{
	if (l == NIL)
		elog(ERROR, "empty list does not have a last item");
	while (lnext(l) != NIL)
		l = lnext(l);
	return lfirst(l);
}

/*
 *	llastnode
 *
 *	Get the last node of l ... NIL if empty list
 */
List *
llastnode(List *l)
{
	if (l == NIL)
		return NIL;
	while (lnext(l) != NIL)
		l = lnext(l);
	return l;
}

/*
 *	freeList
 *
 *	Free the List nodes of a list
 *	The pointed-to nodes, if any, are NOT freed.
 *	This works for integer and Oid lists too.
 */
void
freeList(List *list)
{
	while (list != NIL)
	{
		List	   *l = list;

		list = lnext(list);
		pfree(l);
	}
}

/*
 * equali
 *	  compares two lists of integers
 */
bool
equali(List *list1, List *list2)
{
	List	   *l;

	foreach(l, list1)
	{
		if (list2 == NIL)
			return false;
		if (lfirsti(l) != lfirsti(list2))
			return false;
		list2 = lnext(list2);
	}
	if (list2 != NIL)
		return false;
	return true;
}

/*
 * equalo
 *	  compares two lists of Oids
 */
bool
equalo(List *list1, List *list2)
{
	List	   *l;

	foreach(l, list1)
	{
		if (list2 == NIL)
			return false;
		if (lfirsto(l) != lfirsto(list2))
			return false;
		list2 = lnext(list2);
	}
	if (list2 != NIL)
		return false;
	return true;
}

/*
 * Generate the union of two lists,
 * ie, l1 plus all members of l2 that are not already in l1.
 *
 * NOTE: if there are duplicates in l1 they will still be duplicate in the
 * result; but duplicates in l2 are discarded.
 *
 * The result is a fresh List, but it points to the same member nodes
 * as were in the inputs.
 */
List *
set_union(List *l1, List *l2)
{
	List	   *retval = listCopy(l1);
	List	   *i;

	foreach(i, l2)
	{
		if (!member(lfirst(i), retval))
			retval = lappend(retval, lfirst(i));
	}
	return retval;
}

/* set_union for Oid lists */
List *
set_uniono(List *l1, List *l2)
{
	List	   *retval = listCopy(l1);
	List	   *i;

	foreach(i, l2)
	{
		if (!oidMember(lfirsto(i), retval))
			retval = lappendo(retval, lfirsto(i));
	}
	return retval;
}

/* set_union when pointer-equality comparison is sufficient */
List *
set_ptrUnion(List *l1, List *l2)
{
	List	   *retval = listCopy(l1);
	List	   *i;

	foreach(i, l2)
	{
		if (!ptrMember(lfirst(i), retval))
			retval = lappend(retval, lfirst(i));
	}
	return retval;
}

/*
 * Generate the intersection of two lists,
 * ie, all members of both l1 and l2.
 *
 * NOTE: if there are duplicates in l1 they will still be duplicate in the
 * result; but duplicates in l2 are discarded.
 *
 * The result is a fresh List, but it points to the same member nodes
 * as were in the inputs.
 */
#ifdef NOT_USED
List *
set_intersect(List *l1, List *l2)
{
	List	   *retval = NIL;
	List	   *i;

	foreach(i, l1)
	{
		if (member(lfirst(i), l2))
			retval = lappend(retval, lfirst(i));
	}
	return retval;
}
#endif

/*
 * member()
 *	nondestructive, returns t iff l1 is a member of the list l2
 */
bool
member(void *l1, List *l2)
{
	List	   *i;

	foreach(i, l2)
	{
		if (equal((Node *) l1, (Node *) lfirst(i)))
			return true;
	}
	return false;
}

/*
 * like member(), but use when pointer-equality comparison is sufficient
 */
bool
ptrMember(void *l1, List *l2)
{
	List	   *i;

	foreach(i, l2)
	{
		if (l1 == lfirst(i))
			return true;
	}
	return false;
}

/*
 * membership test for integer lists
 */
bool
intMember(int l1, List *l2)
{
	List	   *i;

	foreach(i, l2)
	{
		if (l1 == lfirsti(i))
			return true;
	}
	return false;
}

/*
 * membership test for Oid lists
 */
bool
oidMember(Oid l1, List *l2)
{
	List	   *i;

	foreach(i, l2)
	{
		if (l1 == lfirsto(i))
			return true;
	}
	return false;
}

/*
 * lremove
 *	  Removes 'elem' from the linked list (destructively changing the list!).
 *	  (If there is more than one equal list member, the first is removed.)
 *
 *	  This version matches 'elem' using simple pointer comparison.
 *	  See also LispRemove.
 */
List *
lremove(void *elem, List *list)
{
	List	   *l;
	List	   *prev = NIL;
	List	   *result = list;

	foreach(l, list)
	{
		if (elem == lfirst(l))
			break;
		prev = l;
	}
	if (l != NIL)
	{
		if (prev == NIL)
			result = lnext(l);
		else
			lnext(prev) = lnext(l);
		pfree(l);
	}
	return result;
}

/*
 *	LispRemove
 *	  Removes 'elem' from the linked list (destructively changing the list!).
 *	  (If there is more than one equal list member, the first is removed.)
 *
 *	  This version matches 'elem' using equal().
 *	  See also lremove.
 */
List *
LispRemove(void *elem, List *list)
{
	List	   *l;
	List	   *prev = NIL;
	List	   *result = list;

	foreach(l, list)
	{
		if (equal(elem, lfirst(l)))
			break;
		prev = l;
	}
	if (l != NIL)
	{
		if (prev == NIL)
			result = lnext(l);
		else
			lnext(prev) = lnext(l);
		pfree(l);
	}
	return result;
}

/*
 *	lremovei
 *		lremove() for integer lists.
 */
List *
lremovei(int elem, List *list)
{
	List	   *l;
	List	   *prev = NIL;
	List	   *result = list;

	foreach(l, list)
	{
		if (elem == lfirsti(l))
			break;
		prev = l;
	}
	if (l != NIL)
	{
		if (prev == NIL)
			result = lnext(l);
		else
			lnext(prev) = lnext(l);
		pfree(l);
	}
	return result;
}

/*
 * ltruncate
 *		Truncate a list to n elements.
 *		Does nothing if n >= length(list).
 *		NB: the list is modified in-place!
 */
List *
ltruncate(int n, List *list)
{
	List	   *ptr;

	if (n <= 0)
		return NIL;				/* truncate to zero length */

	foreach(ptr, list)
	{
		if (--n == 0)
		{
			lnext(ptr) = NIL;
			break;
		}
	}
	return list;
}

/*
 *	set_difference
 *
 *	Return l1 without the elements in l2.
 *
 * The result is a fresh List, but it points to the same member nodes
 * as were in l1.
 */
List *
set_difference(List *l1, List *l2)
{
	List	   *result = NIL;
	List	   *i;

	if (l2 == NIL)
		return listCopy(l1);	/* slightly faster path for empty l2 */

	foreach(i, l1)
	{
		if (!member(lfirst(i), l2))
			result = lappend(result, lfirst(i));
	}
	return result;
}

/*
 *	set_differenceo
 *
 *	Same as set_difference, but for Oid lists
 */
List *
set_differenceo(List *l1, List *l2)
{
	List	   *result = NIL;
	List	   *i;

	if (l2 == NIL)
		return listCopy(l1);	/* slightly faster path for empty l2 */

	foreach(i, l1)
	{
		if (!oidMember(lfirsto(i), l2))
			result = lappendo(result, lfirsto(i));
	}
	return result;
}

/*
 *	set_ptrDifference
 *
 *	Same as set_difference, when pointer-equality comparison is sufficient
 */
List *
set_ptrDifference(List *l1, List *l2)
{
	List	   *result = NIL;
	List	   *i;

	if (l2 == NIL)
		return listCopy(l1);	/* slightly faster path for empty l2 */

	foreach(i, l1)
	{
		if (!ptrMember(lfirst(i), l2))
			result = lappend(result, lfirst(i));
	}
	return result;
}

/*
 * Reverse a list, non-destructively
 */
#ifdef NOT_USED
List *
lreverse(List *l)
{
	List	   *result = NIL;
	List	   *i;

	foreach(i, l)
		result = lcons(lfirst(i), result);
	return result;
}

#endif
