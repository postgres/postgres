/*-------------------------------------------------------------------------
 *
 * qsort.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: qsort.h,v 1.2 1996/11/06 10:29:46 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	QSORT_H
#define	QSORT_H


extern void pg_qsort(void *bot,
		     size_t nmemb,
		     size_t size, 
		     int (*compar)(void *, void *));

#endif	/* QSORT_H */
		     
