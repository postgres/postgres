/*-------------------------------------------------------------------------
 *
 * list.c
 *	  various list handling routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/nodes/list.c,v 1.26 1999/08/14 19:29:35 tgl Exp $
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
 *	makeList
 *
 *	Take varargs, terminated by -1, and make a List
 */
List *
makeList(void *elem,...)
{
	va_list		args;
	List	   *retval = NIL;
	List	   *temp = NIL;
	List	   *tempcons = NIL;

	va_start(args, elem);

	temp = elem;
	while (temp != (void *) -1)
	{
		temp = lcons(temp, NIL);
		if (tempcons == NIL)
			retval = temp;
		else
			lnext(tempcons) = temp;
		tempcons = temp;

		temp = va_arg(args, void *);
	}

	va_end(args);

	return retval;
}

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
 */
Value *
makeFloat(double d)
{
	Value	   *v = makeNode(Value);

	v->type = T_Float;
	v->val.dval = d;
	return v;
}

/*
 *	makeString
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
 *	lappend
 *
 *	Add obj to the end of list, or make a new list if 'list' is NIL
 *
 * MORE EXPENSIVE THAN lcons
 */
List *
lappend(List *list, void *obj)
{
	return nconc(list, lcons(obj, NIL));
}

/*
 *	lappendi
 *
 *	Same as lappend, but for integers
 */
List *
lappendi(List *list, int datum)
{
	return nconc(list, lconsi(datum, NIL));
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
		elog(ERROR, "tryout to nconc a list to itself");

	for (temp = l1; lnext(temp) != NIL; temp = lnext(temp))
		;

	lnext(temp) = l2;
	return l1;					/* list1 is now list1+list2  */
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
	while (n > 0)
	{
		l = lnext(l);
		n--;
	}
	return lfirst(l);
}

/*
 *	nthi
 *
 *	Same as nthi, but for integers
 */
int
nthi(int n, List *l)
{
	/* XXX assume list is long enough */
	while (n > 0)
	{
		l = lnext(l);
		n--;
	}
	return lfirsti(l);
}

/* this is here solely for rt_store. Get rid of me some day! */
void
set_nth(List *l, int n, void *elem)
{
	/* XXX assume list is long enough */
	while (n > 0)
	{
		l = lnext(l);
		n--;
	}
	lfirst(l) = elem;
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
 *	freeList
 *
 *	Free the List nodes of a list
 *  The pointed-to nodes, if any, are NOT freed.
 *  This works for integer lists too.
 * 
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
 *		same
 *
 *		Returns t if two lists contain the same elements
 *		(but unlike equal(), they need not be in the same order)
 *	    
 *
 * XXX should be called samei() --- only good for IntList	-ay
 */
bool
same(List *l1, List *l2)
{
	List	   *temp;

	if (l1 == NIL)
		return l2 == NIL;
	if (l2 == NIL)
		return l1 == NIL;
	if (length(l1) == length(l2))
	{
		foreach(temp, l1)
		{
			if (!intMember(lfirsti(temp), l2))
				return false;
		}
		return true;
	}
	return false;

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
LispUnion(List *l1, List *l2)
{
	List	   *retval = listCopy(l1);
	List	   *i;

	foreach(i, l2)
	{
		if (! member(lfirst(i), retval))
			retval = lappend(retval, lfirst(i));
	}
	return retval;
}

List *
LispUnioni(List *l1, List *l2)
{
	List	   *retval = listCopy(l1);
	List	   *i;

	foreach(i, l2)
	{
		if (! intMember(lfirsti(i), retval))
			retval = lappendi(retval, lfirsti(i));
	}
	return retval;
}

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
 * lremove
 *	  Removes 'elem' from the the linked list.
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
	}
	return result;
}

/*
 *	LispRemove
 *	  Removes 'elem' from the the linked list.
 *	  This version matches 'elem' using equal().
 *	  (If there is more than one equal list member, the first is removed.)
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
	}
	return result;
}

#ifdef NOT_USED

List *
intLispRemove(int elem, List *list)
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
	}
	return result;
}

#endif

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
		if (! member(lfirst(i), l2))
			result = lappend(result, lfirst(i));
	}
	return result;
}

/*
 *	set_differencei
 *
 *	Same as set_difference, but for integers
 */
List *
set_differencei(List *l1, List *l2)
{
	List	   *result = NIL;
	List	   *i;

	if (l2 == NIL)
		return listCopy(l1);	/* slightly faster path for empty l2 */

	foreach(i, l1)
	{
		if (! intMember(lfirsti(i), l2))
			result = lappendi(result, lfirsti(i));
	}
	return result;
}
