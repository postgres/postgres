/*-------------------------------------------------------------------------
 *
 * stringutils.c--
 *    simple string manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/stringutils.c,v 1.2 1996/07/28 07:08:15 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "stringutils.h"

/* all routines assume null-terminated strings! */

/* removes whitespaces from the left, right and both sides of a string */
/* MODIFIES the string passed in and returns the head of it */
char *leftTrim(char *s)  
{
  char *s2 = s;
  int shift=0;
  int j=0;

  while (isspace(*s))
    { s++; shift++;}
  if (shift > 0)
    {
      while ( (s2[j] = s2[j+shift]) !='\0')
	j++;
    }

  return s2;
}

char *rightTrim(char *s)
{
  char *sEnd;
  sEnd = s+strlen(s)-1;
  while (isspace(*sEnd))
    sEnd--;
  if (sEnd < s)
    s[0]='\0';
  else
    s[sEnd-s+1]='\0';
  return s;
}

char *doubleTrim(char *s)
{
  strcpy(s,leftTrim(rightTrim(s)));
  return s;
}

/* dupstr : copies a string, while allocating space for it. 
   the CALLER is responsible for freeing the space
   returns NULL if the argument is NULL */
char *dupstr(char *s)
{
  char *result;

  if (s == NULL)
    return NULL;

  result = (char*)malloc(strlen(s)+1);
  strcpy(result, s);
  return result;
}


#ifdef STRINGUTILS_TEST
void testStringUtils()
{
  static char *tests[] = {" goodbye  \n", /* space on both ends */
			  "hello world",  /* no spaces to trim */
			  "",		/* empty string */
			  "a",		/* string with one char*/
			  " ",		/* string with one whitespace*/
			  NULL_STR};

  int i=0;
  while (tests[i]!=NULL_STR)
    {
      char *t;
      t = dupstr(tests[i]);
      printf("leftTrim(%s) = ",t);
      printf("%sEND\n", leftTrim(t));
      t = dupstr(tests[i]);
      printf("rightTrim(%s) = ",t);
      printf("%sEND\n", rightTrim(t));
      t = dupstr(tests[i]);
      printf("doubleTrim(%s) = ",t);
      printf("%sEND\n", doubleTrim(t));
      i++;
    }

}

#endif
