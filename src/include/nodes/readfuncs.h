/*-------------------------------------------------------------------------
 *
 * readfuncs.h--
 *    header file for read.c and readfuncs.c. These functions are internal
 *    to the stringToNode interface and should not be used by anyone else.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: readfuncs.h,v 1.1 1996/08/28 01:57:47 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	READFUNCS_H
#define	READFUNCS_H

/*
 * prototypes for functions in read.c (the lisp token parser)
 */
extern char *lsptok(char *string, int *length);
extern void *nodeRead(bool read_car_only);

/*
 * prototypes for functions in readfuncs.c 
 */
extern Node *parsePlanString();

#endif	/* READFUNCS_H */
