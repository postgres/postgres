/*-------------------------------------------------------------------------
 *
 *	 FILE
 *		fe-misc.c
 *
 *	 DESCRIPTION
 *		 miscellaneous useful functions
 *	 these routines are analogous to the ones in libpq/pqcomm.c
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-misc.c,v 1.10 1998/02/26 04:45:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <stdio.h>

#include "postgres.h"

#include "libpq-fe.h"

/* --------------------------------------------------------------------- */
/* pqGetc:
   get a character from stream f

   if debug is set, also echo the character fetched
*/
int
pqGetc(FILE *fin, FILE *debug)
{
	int			c;

	c = getc(fin);

	if (debug && c != EOF)
		fprintf(debug, "From backend> %c\n", c);

	return c;
}

/* --------------------------------------------------------------------- */
/* pqPutnchar:
   send a string of exactly len length into stream f

   returns 1 if there was an error, 0 otherwise.
*/
int
pqPutnchar(const char *s, int len, FILE *f, FILE *debug)
{
	if (debug)
		fprintf(debug, "To backend> %s\n", s);

	return (pqPutNBytes(s, len, f) == EOF ? 1 : 0);
}

/* --------------------------------------------------------------------- */
/* pqGetnchar:
   get a string of exactly len bytes in buffer s (which must be 1 byte
   longer) from stream f and terminate it with a '\0'.
*/
int
pqGetnchar(char *s, int len, FILE *f, FILE *debug)
{
	int			status;

	status = pqGetNBytes(s, len, f);

	if (debug)
		fprintf(debug, "From backend (%d)> %s\n", len, s);

	return (status == EOF ? 1 : 0);
}

/* --------------------------------------------------------------------- */
/* pqGets:
   get a string of up to length len from stream f
*/
int
pqGets(char *s, int len, FILE *f, FILE *debug)
{
	int			status;

	status = pqGetString(s, len, f);

	if (debug)
		fprintf(debug, "From backend> \"%s\"\n", s);

	return (status == EOF ? 1 : 0);
}

/* --------------------------------------------------------------------- */
/* pgPutInt
   send an integer of 2 or 4 bytes to the file stream, compensate
   for host endianness.
   returns 0 if successful, 1 otherwise
*/
int
pqPutInt(const int integer, int bytes, FILE *f, FILE *debug)
{
	int			retval = 0;

	switch (bytes)
	{
		case 2:
			retval = pqPutShort(integer, f);
			break;
		case 4:
			retval = pqPutLong(integer, f);
			break;
		default:
			fprintf(stderr, "** int size %d not supported\n", bytes);
			retval = 1;
	}

	if (debug)
		fprintf(debug, "To backend (%d#)> %d\n", bytes, integer);

	return retval;
}

/* --------------------------------------------------------------------- */
/* pgGetInt
   read a 2 or 4 byte integer from the stream and swab it around
   to compensate for different endianness
   returns 0 if successful
*/
int
pqGetInt(int *result, int bytes, FILE *f, FILE *debug)
{
	int			retval = 0;

	switch (bytes)
	{
		case 2:
			retval = pqGetShort(result, f);
			break;
		case 4:
			retval = pqGetLong(result, f);
			break;
		default:
			fprintf(stderr, "** int size %d not supported\n", bytes);
			retval = 1;
	}

	if (debug)
		fprintf(debug, "From backend (#%d)> %d\n", bytes, *result);

	return retval;
}

/* --------------------------------------------------------------------- */
int
pqPuts(const char *s, FILE *f, FILE *debug)
{
	if (pqPutString(s, f) == EOF)
		return 1;

	fflush(f);

	if (debug)
		fprintf(debug, "To backend> %s\n", s);

	return 0;
}

/* --------------------------------------------------------------------- */
void
pqFlush(FILE *f, FILE *debug)
{
	if (f)
		fflush(f);

	if (debug)
		fflush(debug);
}

/* --------------------------------------------------------------------- */
