/*-------------------------------------------------------------------------
 *
 * lispsort.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lispsort.h,v 1.1.1.1 1996/07/09 06:21:29 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	LISPSORT_H
#define	LISPSORT_H

extern List *lisp_qsort(List *the_list, int (*compare)());

#endif	/* LISPSORT_H */
