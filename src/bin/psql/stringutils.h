/*-------------------------------------------------------------------------
 *
 * stringutils.h--
 *    
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: stringutils.h,v 1.2 1996/07/28 07:08:15 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRINGUTILS_H
#define STRINGUTILS_H

/* use this for memory checking of alloc and free using Tcl's memory check
  package*/
#ifdef TCL_MEM_DEBUG
#include <tcl.h>
#define malloc(x) ckalloc(x)
#define free(x) ckfree(x)
#define realloc(x,y) ckrealloc(x,y)
#endif

/* string fiddling utilties */

/* all routines assume null-terminated strings! as arguments */

/* removes whitespaces from the left, right and both sides of a string */
/* MODIFIES the string passed in and returns the head of it */
extern char *leftTrim(char *s);  
extern char *rightTrim(char *s);
extern char *doubleTrim(char *s);

/* dupstr : copies a string, while making room for it */
/* the CALLER is responsible for freeing the space */
/* returns NULL if the argument is NULL */
extern char *dupstr(char *s);

#ifdef STRINGUTILS_TEST
extern void testStringUtils();
#endif

#ifndef NULL_STR
#define NULL_STR (char*)0
#endif

#ifndef NULL
#define NULL 0
#endif

#endif /* STRINGUTILS_H */
