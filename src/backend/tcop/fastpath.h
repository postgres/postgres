/*-------------------------------------------------------------------------
 *
 * fastpath.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fastpath.h,v 1.1.1.1 1996/07/09 06:21:59 scrappy Exp $
 *
 * NOTES
 *    This information pulled out of tcop/fastpath.c and put
 *    here so that the PQfn() in be-pqexec.c could access it.
 *	-cim 2/26/91
 *
 *-------------------------------------------------------------------------
 */
#ifndef FASTPATH_H
#define FASTPATH_H

/* ----------------
 *	fastpath #defines
 * ----------------
 */
#define VAR_LENGTH_RESULT 	(-1)
#define VAR_LENGTH_ARG 		(-5)
#define MAX_STRING_LENGTH 	256

extern int HandleFunctionRequest(void);

#endif	/* FASTPATH_H */
