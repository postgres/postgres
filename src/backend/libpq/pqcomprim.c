#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "postgres.h"
#include "miscadmin.h"
#include "libpq/pqcomm.h"
#include "libpq/libpq.h"


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

/*
#define ntoh_s(n)	(uint16)(((u_char *)&n)[1] << 8 \
			  | ((u_char *)&n)[0])
#define ntoh_l(n)	(uint32)(((u_char *)&n)[3] << 24 \
			  | ((u_char *)&n)[2] << 16 \
			  | ((u_char *)&n)[1] <<  8 \
			  | ((u_char *)&n)[0])
*/
#define ntoh_s(n)	(uint16)((((uint16)n & 0x00ff) <<  8) | \
				 (((uint16)n & 0xff00) >>  8))
#define ntoh_l(n)	(uint32)((((uint32)n & 0x000000ff) << 24) | \
				 (((uint32)n & 0x0000ff00) <<  8) | \
				 (((uint32)n & 0x00ff0000) >>  8) | \
				 (((uint32)n & 0xff000000) >> 24))
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
pqPutShort(int integer)
{
	uint16		n;

#ifdef FRONTEND
	n = htons((uint16) integer);
#else
	n = ((PG_PROTOCOL_MAJOR(FrontendProtocol) == 0) ? hton_s(integer) : htons((uint16) integer));
#endif

	return pqPutNBytes((char *)&n, 2); /* 0 on success, EOF otherwise */
}

/* --------------------------------------------------------------------- */
int
pqPutLong(int integer)
{
	uint32		n;

#ifdef FRONTEND
	n = htonl((uint32) integer);
#else
	n = ((PG_PROTOCOL_MAJOR(FrontendProtocol) == 0) ? hton_l(integer) : htonl((uint32) integer));
#endif

        return pqPutNBytes((char *)&n,4);
}

/* --------------------------------------------------------------------- */
int
pqGetShort(int *result)
{
	uint16		n;

	if (pqGetNBytes((char *)&n,2) != 0)
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
pqGetLong(int *result)
{
	uint32		n;

	if (pqGetNBytes((char *)&n, 4) != 0)
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
pqGetNBytes(char *s, size_t len)
{
	int bytesDone = 0;

	do {
	  int r = recv(MyProcPort->sock, s+bytesDone, len-bytesDone, 0);
	  if (r == 0 || r == -1) {
	    if (errno != EINTR)
	      return EOF; /* All other than signal-interrupted is error */
	    continue; /* Otherwise, try again */
	  }
	  
	  /* r contains number of bytes received */
	  bytesDone += r;

	} while (bytesDone < len);
	/* Zero-termination now in pq_getnchar() instead */
	return 0;
}

/* --------------------------------------------------------------------- */
int
pqPutNBytes(const char *s, size_t len)
{
        int bytesDone = 0;

	do {
	  int r = send(MyProcPort->sock, s+bytesDone, len-bytesDone, 0);
	  if (r == 0 || r == -1) {
	    if (errno != EINTR)
	      return EOF; /* Only signal interruption allowed */
	    continue; /* If interruped and read nothing, just try again */
	  }
	  
	  /* r contains number of bytes sent so far */
	  bytesDone += r;
	} while (bytesDone < len);

	return 0;
}

/* --------------------------------------------------------------------- */
int
pqGetString(char *s, size_t len)
{
	int			c;

	/*
	 * Keep on reading until we get the terminating '\0' and discard those
	 * bytes we don't have room for.
	 */

	while ((c = pq_getchar()) != EOF && c != '\0')
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
pqPutString(const char *s)
{
  return pqPutNBytes(s,strlen(s)+1);
}

