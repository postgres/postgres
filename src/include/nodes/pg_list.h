/*-------------------------------------------------------------------------
 *
 * pg_list.h--
 *    POSTGRES generic list package
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_list.h,v 1.3 1996/11/04 07:18:19 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PG_LIST_H
#define	PG_LIST_H

#include <nodes/nodes.h>

/* ----------------------------------------------------------------
 *			node definitions
 * ----------------------------------------------------------------
 */

/*----------------------
 * 	Value node
 *----------------------
 */
typedef struct Value {
    NodeTag		type;	/* tag appropriately (eg. T_String) */
    union ValUnion {
	char   		*str;	/* string */ 
	long   		ival;
	double 		dval;
    } val;
} Value;

#define	intVal(v)	(((Value *)v)->val.ival)
#define	floatVal(v)	(((Value *)v)->val.dval)
#define strVal(v)	(((Value *)v)->val.str)


/*----------------------
 * 	List node
 *----------------------
 */
typedef	struct List {
    NodeTag		type;
    void		*elem;
    struct List		*next;
} List;

#define    NIL		((List *) NULL)

/* ----------------
 *	accessor macros
 * ----------------
 */
#define lfirst(l)				((l)->elem)
#define lnext(l)				((l)->next)
#define lsecond(l)				(lfirst(lnext(l)))

/*
 * foreach -
 *    a convenience macro which loops through the list
 */
#define foreach(_elt_,_list_)   \
    for(_elt_=_list_; _elt_!=NIL;_elt_=lnext(_elt_))


/*
 * function prototypes in nodes/list.c
 */
extern int length(List *list);
extern List *append(List *list1, List *list2);
extern List *nconc(List *list1, List *list2);
extern List *lcons(void *datum, List *list);
extern bool member(void *foo, List *bar);
extern Value *makeInteger(long i);
extern Value *makeFloat(double d);
extern Value *makeString(char *str);
extern List *makeList(void *elem, ...);
extern List *lappend(List *list, void *obj);
extern List *lremove(void *elem, List *list);
extern void freeList(List *list);
     
extern void *nth(int n, List *l);
extern void set_nth(List *l, int n, void *elem);
		    
/* hack for now */
#define	lconsi(i,l)	lcons((void*)(int)i,l)
#define lfirsti(l)	((int)lfirst(l))
#define	lappendi(l,i)	lappend(l,(void*)i)
extern bool intMember(int, List *);
extern List *intAppend(List *list1, List *list2);

extern List *nreverse(List *);
extern List *set_difference(List *, List *);
extern List *set_differencei(List *, List *);
extern List *LispRemove(void *, List *);
extern List *intLispRemove(int, List *);
extern List *LispUnion(List *foo, List *bar);
extern List *LispUnioni(List *foo, List *bar);
extern bool same(List *foo, List *bar);

/* should be in nodes.h but needs List */
extern bool equali(List *a, List *b);

/* in copyfuncs.c */
extern List *listCopy(List *);

#endif /* PG_LIST_H */
