/*-------------------------------------------------------------------------
 *
 * pg_list.h
 *	  POSTGRES generic list package
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_list.h,v 1.41 2003/08/22 20:34:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LIST_H
#define PG_LIST_H

#include "nodes/nodes.h"

/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/*----------------------
 *		Value node
 *
 * The same Value struct is used for five node types: T_Integer,
 * T_Float, T_String, T_BitString, T_Null.
 *
 * Integral values are actually represented by a machine integer,
 * but both floats and strings are represented as strings.
 * Using T_Float as the node type simply indicates that
 * the contents of the string look like a valid numeric literal.
 *
 * (Before Postgres 7.0, we used a double to represent T_Float,
 * but that creates loss-of-precision problems when the value is
 * ultimately destined to be converted to NUMERIC.	Since Value nodes
 * are only used in the parsing process, not for runtime data, it's
 * better to use the more general representation.)
 *
 * Note that an integer-looking string will get lexed as T_Float if
 * the value is too large to fit in a 'long'.
 *
 * Nulls, of course, don't need the value part at all.
 *----------------------
 */
typedef struct Value
{
	NodeTag		type;			/* tag appropriately (eg. T_String) */
	union ValUnion
	{
		long		ival;		/* machine integer */
		char	   *str;		/* string */
	}			val;
} Value;

#define intVal(v)		(((Value *)(v))->val.ival)
#define floatVal(v)		atof(((Value *)(v))->val.str)
#define strVal(v)		(((Value *)(v))->val.str)


/*----------------------
 *		List node
 *
 * We support three types of lists:
 *	lists of pointers (in practice always pointers to Nodes, but declare as
 *		"void *" to minimize casting annoyances)
 *	lists of integers
 *	lists of Oids
 *
 * (At this writing, ints and Oids are the same size, but they may not always
 * be so; try to be careful to maintain the distinction.)
 *----------------------
 */
typedef struct List
{
	NodeTag		type;
	union
	{
		void	   *ptr_value;
		int			int_value;
		Oid			oid_value;
	}			elem;
	struct List *next;
} List;

#define    NIL			((List *) NULL)

/* ----------------
 *		accessor macros
 *
 * The general naming convention is that the base name xyz() is for the
 * pointer version, xyzi() is for integers, xyzo() is for Oids.  We don't
 * bother with multiple names if the same routine can handle all cases.
 * ----------------
 */

#define lfirst(l)								((l)->elem.ptr_value)
#define lfirsti(l)								((l)->elem.int_value)
#define lfirsto(l)								((l)->elem.oid_value)

#define lnext(l)								((l)->next)

#define lsecond(l)								lfirst(lnext(l))

#define lthird(l)								lfirst(lnext(lnext(l)))

#define lfourth(l)								lfirst(lnext(lnext(lnext(l))))

/*
 * foreach -
 *	  a convenience macro which loops through the list
 */
#define foreach(_elt_,_list_)	\
	for (_elt_ = (_list_); _elt_ != NIL; _elt_ = lnext(_elt_))

/*
 * Convenience macros for building fixed-length lists
 */
#define makeList1(x1)				lcons(x1, NIL)
#define makeList2(x1,x2)			lcons(x1, makeList1(x2))
#define makeList3(x1,x2,x3)			lcons(x1, makeList2(x2,x3))
#define makeList4(x1,x2,x3,x4)		lcons(x1, makeList3(x2,x3,x4))

#define makeListi1(x1)				lconsi(x1, NIL)
#define makeListi2(x1,x2)			lconsi(x1, makeListi1(x2))

#define makeListo1(x1)				lconso(x1, NIL)
#define makeListo2(x1,x2)			lconso(x1, makeListo1(x2))

/*
 * FastList is an optimization for building large lists.  The conventional
 * way to build a list is repeated lappend() operations, but that is O(N^2)
 * in the number of list items, which gets tedious for large lists.
 *
 * Note: there are some hacks in gram.y that rely on the head pointer (the
 * value-as-list) being the first field.
 */
typedef struct FastList
{
	List	   *head;
	List	   *tail;
} FastList;

#define FastListInit(fl)	( (fl)->head = (fl)->tail = NIL )
#define FastListFromList(fl, l)  \
	( (fl)->head = (l), (fl)->tail = llastnode((fl)->head) )
#define FastListValue(fl)	( (fl)->head )

#define makeFastList1(fl, x1)  \
	( (fl)->head = (fl)->tail = makeList1(x1) )


/*
 * function prototypes in nodes/list.c
 */
extern Value *makeInteger(long i);
extern Value *makeFloat(char *numericStr);
extern Value *makeString(char *str);
extern Value *makeBitString(char *str);

extern List *lcons(void *datum, List *list);
extern List *lconsi(int datum, List *list);
extern List *lconso(Oid datum, List *list);
extern List *lappend(List *list, void *datum);
extern List *lappendi(List *list, int datum);
extern List *lappendo(List *list, Oid datum);
extern List *nconc(List *list1, List *list2);
extern void FastAppend(FastList *fl, void *datum);
extern void FastAppendi(FastList *fl, int datum);
extern void FastAppendo(FastList *fl, Oid datum);
extern void FastConc(FastList *fl, List *cells);
extern void FastConcFast(FastList *fl, FastList *fl2);
extern void *nth(int n, List *l);
extern int	length(List *list);
extern void *llast(List *list);
extern List *llastnode(List *list);
extern bool member(void *datum, List *list);
extern bool ptrMember(void *datum, List *list);
extern bool intMember(int datum, List *list);
extern bool oidMember(Oid datum, List *list);
extern List *lremove(void *elem, List *list);
extern List *LispRemove(void *elem, List *list);
extern List *lremovei(int elem, List *list);
extern List *ltruncate(int n, List *list);

extern List *set_union(List *list1, List *list2);
extern List *set_uniono(List *list1, List *list2);
extern List *set_ptrUnion(List *list1, List *list2);
extern List *set_difference(List *list1, List *list2);
extern List *set_differenceo(List *list1, List *list2);
extern List *set_ptrDifference(List *list1, List *list2);

extern bool equali(List *list1, List *list2);
extern bool equalo(List *list1, List *list2);

extern void freeList(List *list);

/* in copyfuncs.c */
extern List *listCopy(List *list);

#endif   /* PG_LIST_H */
