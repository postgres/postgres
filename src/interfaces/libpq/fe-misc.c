/*-------------------------------------------------------------------------
 *
 *   FILE
 *	fe-misc.c
 *
 *   DESCRIPTION
 *       miscellaneous useful functions
 *   these routines are analogous to the ones in libpq/pqcomm.c
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-misc.c,v 1.4 1996/12/31 07:29:17 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <stdio.h>

#include "postgres.h"

#include "libpq-fe.h"

/* pqGetc:
   get a character from stream f

   if debug is set, also echo the character fetched
*/
int
pqGetc(FILE* fin, FILE* debug)
{
  int c;

  c = getc(fin);
  if (debug && c != EOF)
    fprintf(debug, "From backend> %c\n", c);
  return c;
}

/* pqPutnchar:
   send a string of exactly len length into stream f 

   returns 1 if there was an error, 0 otherwise.
*/
int
pqPutnchar(const char* s, int len, FILE *f, FILE *debug)
{
    int status;

    if (f == NULL)
	return 1;

    if (debug) fputs("To backend>", debug);
    while (len--) {
	status = fputc(*s,f);
	if (debug)
	    fputc(*s,debug);
	s++;
	if (status == EOF)
	    return 1;
    }
    if (debug) fputc('\n', debug);
    return 0;
}

/* pqGetnchar:
   get a string of exactly len length from stream f 
*/
int
pqGetnchar(char* s, int len, FILE *f, FILE *debug)
{
  int c;

  if (f == NULL)
    return 1;
  
  while (len-- && (c = getc(f)) != EOF)
    *s++ = c;
  *s = '\0';

  if (debug) {
      fprintf(debug, "From backend> %s\n", s);
  }
  return 0;
}

/* pqGets:
   get a string of up to length len from stream f
*/
int
pqGets(char* s, int len, FILE *f, FILE *debug)
{
  int c;

  if (f == NULL)
    return 1;
  
  while (len-- && (c = getc(f)) != EOF && c)
    *s++ = c;
  *s = '\0';

  if (debug) {
      fprintf(debug, "From backend> %s\n", s);
  }
  return 0;
}


/* pgPutInt
   send an integer of up to 4 bytesto the file stream
   do this one byte at at time. 
   This insures that machines with different ENDIANness can talk to each other
   get a n-byte integer from the stream into result 
   returns 0 if successful, 1 otherwise
*/
int
pqPutInt(const int integer, int bytes, FILE* f, FILE *debug)
{
    int i;
    int status;

    i = integer;

    if (bytes > 4)
	bytes = 4;

    while (bytes--) {
	status = fputc(i & 0xff, f);
	i >>= 8;
	if (status == EOF) {
	    return 1;
	}
    }
    if (debug) fprintf(debug, "To backend (#)> %d\n", integer);
    return 0;
}

/* pgGetInt 
   reconstructs the integer one byte at a time.
   This insures that machines with different ENDIANness can talk to each other
   get a n-byte integer from the stream into result 
   returns 0 if successful 
*/
int
pqGetInt(int* result, int bytes, FILE* f, FILE *debug)
{
  int c;
  int p;
  int n;

  if (f == NULL)
    return 1;
  
  p = 0;
  n = 0;
  while (bytes && (c = getc(f)) != EOF)
    {
      n |= (c & 0xff) << p;
      p += 8;
      bytes--;
    }

  if (bytes != 0)
    return 1;

  *result = n;
  if (debug)
      fprintf(debug,"From backend (#)> %d\n",*result);
  return 0;
}


int
pqPuts(const char* s, FILE *f, FILE *debug)
{
  if (f == NULL)
    return 1;
  
  if (fputs(s,f) == EOF)
    return 1;

  fputc('\0',f); /* important to send an ending EOF since backend expects it */
  fflush(f);

  if (debug) {
      fprintf(debug, "To backend> %s\n", s);
  }
  return 0;
}


void
pqFlush(FILE *f, FILE *debug)
{
    if (f)
	fflush(f);
    if (debug)
	fflush(debug);
}


