/*-------------------------------------------------------------------------
 *
 * qsort.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: qsort.h,v 1.1 1996/08/28 07:22:40 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	QSORT_H
#define	QSORT_H

#include <sys/types.h>

extern void pg_qsort(void *bot,
		     size_t nmemb,
		     size_t size, 
		     int (*compar)(void *, void *));

#endif	/* QSORT_H */
		     
