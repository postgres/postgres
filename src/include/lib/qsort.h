/*-------------------------------------------------------------------------
 *
 * qsort.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: qsort.h,v 1.7 1999/02/13 23:21:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef QSORT_H
#define QSORT_H


extern void pg_qsort(void *bot,
		 size_t nmemb,
		 size_t size,
		 int (*compar) (void *, void *));

#endif	 /* QSORT_H */
