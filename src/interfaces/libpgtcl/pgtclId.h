/*-------------------------------------------------------------------------
 *
 * pgtclId.h--
 *    useful routines to convert between strings and pointers
 *  Needed because everything in tcl is a string, but often, pointers
 *  to data structures are needed.
 *    
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pgtclId.h,v 1.1.1.1 1996/07/09 06:22:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

extern void PgSetId(char *id, void *ptr);
extern void* PgGetId(char *id);
extern int PgValidId(char* id);
