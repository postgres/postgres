/*-------------------------------------------------------------------------
 *
 * tqual.h--
 *	  POSTGRES "time" qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tqual.h,v 1.11 1997/11/20 23:24:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TQUAL_H
#define TQUAL_H

#include <access/htup.h>

/* As above, plus updates in this command */

extern void setheapoverride(bool on);
extern bool heapisoverride(void);

extern bool HeapTupleSatisfiesVisibility(HeapTuple tuple, bool seeself);


#endif							/* TQUAL_H */
