#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "postgres.h"
#include "libpq/pqcomm.h"


/*
 * The backend supports the old little endian byte order and the current
 * network byte order.
 */

#ifndef FRONTEND

#include "libpq/libpq-be.h"

#ifdef HAVE_ENDIAN_H
#include <endian.h>
#endif

#ifndef BYTE_ORDER
#error BYTE_ORDER must be defined as LITTLE_ENDIAN, BIG_ENDIAN or PDP_ENDIAN
#endif

#if BYTE_ORDER == LITTLE_ENDIAN

#define ntoh_s(n)	n
#define ntoh_l(n)	n
#define hton_s(n)	n
#define hton_l(n)	n

#else
#if BYTE_ORDER == BIG_ENDIAN

#define ntoh_s(n)	(uint16)(((u_char *)&n)[1] << 8 \
			  | ((u_char *)&n)[0])
#define ntoh_l(n)	(uint32)(((u_char *)&n)[3] << 24 \
			  | ((u_char *)&n)[2] << 16 \
			  | ((u_char *)&n)[1] <<  8 \
			  | ((u_char *)&n)[0])
#define hton_s(n)	(ntoh_s(n))
#define hton_l(n)	(ntoh_l(n))

#else
#if BYTE_ORDER == PDP_ENDIAN

#error PDP_ENDIAN macros not written yet

#else

#error BYTE_ORDER not defined as anything understood

#endif
#endif
#endif

#endif


/* --------------------------------------------------------------------- */
int
pqPutShort(int integer, FILE *f)
{
	uint16		n;

#ifdef FRONTEND
	n = htons((uint16) integer);
#else
	n = ((PG_PROTOCOL_MAJOR(FrontendProtocol) == 0) ? hton_s(integer) : htons((uint16) integer));
#endif

	if (fwrite(&n, 2, 1, f) != 1)
		return EOF;

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqPutLong(int integer, FILE *f)
{
	uint32		n;

#ifdef FRONTEND
	n = htonl((uint32) integer);
#else
	n = ((PG_PROTOCOL_MAJOR(FrontendProtocol) == 0) ? hton_l(integer) : htonl((uint32) integer));
#endif

	if (fwrite(&n, 4, 1, f) != 1)
		return EOF;

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqGetShort(int *result, FILE *f)
{
	uint16		n;

	if (fread(&n, 2, 1, f) != 1)
		return EOF;

#ifdef FRONTEND
	*result = (int) ntohs(n);
#else
	*result = (int) ((PG_PROTOCOL_MAJOR(FrontendProtocol) == 0) ? ntoh_s(n) : ntohs(n));
#endif

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqGetLong(int *result, FILE *f)
{
	uint32		n;

	if (fread(&n, 4, 1, f) != 1)
		return EOF;

#ifdef FRONTEND
	*result = (int) ntohl(n);
#else
	*result = (int) ((PG_PROTOCOL_MAJOR(FrontendProtocol) == 0) ? ntoh_l(n) : ntohl(n));
#endif

	return 0;
}

/* --------------------------------------------------------------------- */
/* pqGetNBytes: Read a chunk of exactly len bytes in buffer s (which must be 1
		byte longer) and terminate it with a '\0'.
		Return 0 if ok.
*/
int
pqGetNBytes(char *s, size_t len, FILE *f)
{
	int			cnt;

	if (f == NULL)
		return EOF;

	cnt = fread(s, 1, len, f);
	s[cnt] = '\0';

	return (cnt == len) ? 0 : EOF;
}

/* --------------------------------------------------------------------- */
int
pqPutNBytes(const char *s, size_t len, FILE *f)
{
	if (f == NULL)
		return EOF;

	if (fwrite(s, 1, len, f) != len)
		return EOF;

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqGetString(char *s, size_t len, FILE *f)
{
	int			c;

	if (f == NULL)
		return EOF;

	/*
	 * Keep on reading until we get the terminating '\0' and discard those
	 * bytes we don't have room for.
	 */

	while ((c = getc(f)) != EOF && c != '\0')
		if (len > 1)
		{
			*s++ = c;
			len--;
		}

	*s = '\0';

	if (c == EOF)
		return EOF;

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqPutString(const char *s, FILE *f)
{
	if (f == NULL)
		return 0;

	if (fputs(s, f) == EOF)
		return EOF;

	fputc('\0', f);				/* important to send an ending \0 since
								 * backend expects it */

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqGetByte(FILE *f)
{
	return getc(f);
}

/* --------------------------------------------------------------------- */
int
pqPutByte(int c, FILE *f)
{
	if (!f)
		return 0;

	return (putc(c, f) == c) ? 0 : EOF;
}
