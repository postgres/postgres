#include <stdlib.h>
#include <stdio.h>

#include "postgres.h"
#include "libpq/pqcomm.h"

#ifdef        HAVE_ENDIAN_H
#  include    <endian.h>
#endif


/* --------------------------------------------------------------------- */
/* Is the other way around than system ntoh/hton, so we roll our own
	here */
	
#ifndef		BYTE_ORDER
#error BYTE_ORDER must be defined as LITTLE_ENDIAN, BIG_ENDIAN or PDP_ENDIAN
#endif

#if BYTE_ORDER == LITTLE_ENDIAN
#  define ntoh_s(n) n
#  define ntoh_l(n) n
#  define hton_s(n) n
#  define hton_l(n) n
#else	/* BYTE_ORDER != LITTLE_ENDIAN */
#  if BYTE_ORDER == BIG_ENDIAN
#    define ntoh_s(n) (u_short)(((u_char *) &n)[0] << 8 | ((u_char *) &n)[1])
#    define ntoh_l(n) (u_long)(((u_char *)&n)[0] << 24 | \
							((u_char *)&n)[1] << 16 | \
      	             	   ((u_char *)&n)[2] << 8 | ((u_char *)&n)[3])
#    define hton_s(n) (u_short)(((u_char *) &n)[2] << 8 | ((u_char *) &n)[3])
#    define hton_l(n) (ntoh_l(n))
#  else	/* BYTE_ORDER != BIG_ENDIAN */
#    if BYTE_ORDER == PDP_ENDIAN
#      #error PDP_ENDIAN macros not written yet
#    else	/* BYTE_ORDER !=  anything known */
#      #error BYTE_ORDER not defined as anything understood
#    endif	/* BYTE_ORDER == PDP_ENDIAN */
#  endif	/* BYTE_ORDER == BIG_ENDIAN */
#endif		/* BYTE_ORDER == LITTLE_ENDIAN */

/* --------------------------------------------------------------------- */
int pqPutShort(int integer, FILE *f)
    {
    int retval = 0;
    u_short n;
		
    n = hton_s(integer);
    if(fwrite(&n, sizeof(u_short), 1, f) != 1)
    	retval = EOF;
    
    return retval;
    }

/* --------------------------------------------------------------------- */
int pqPutLong(int integer, FILE *f)
    {
    int retval = 0;
    u_long n;
		
    n = hton_l(integer);
    if(fwrite(&n, sizeof(u_long), 1, f) != 1)
    	retval = EOF;
    
    return retval;
    }
    
/* --------------------------------------------------------------------- */
int pqGetShort(int *result, FILE *f)
    {
    int retval = 0;
    u_short n;

    if(fread(&n, sizeof(u_short), 1, f) != 1)
    	retval = EOF;
			
    *result = ntoh_s(n);
    return retval;
    }

/* --------------------------------------------------------------------- */
int pqGetLong(int *result, FILE *f)
    {
    int retval = 0;
    u_long n;
		
    if(fread(&n, sizeof(u_long), 1, f) != 1)
    	retval = EOF;
			
    *result = ntoh_l(n);
    return retval;
    }

/* --------------------------------------------------------------------- */
/* pqGetNBytes: Read a chunk of exactly len bytes in buffer s.
	Return 0 if ok.
*/
int pqGetNBytes(char *s, size_t len, FILE *f)
	{
	int cnt;

	if (f == NULL)
		return EOF;
  
	cnt = fread(s, 1, len, f);
	s[cnt] = '\0';
   /* mjl: actually needs up to len+1 bytes, is this okay? XXX */

	return (cnt == len) ? 0 : EOF;
	}

/* --------------------------------------------------------------------- */
int pqPutNBytes(const char *s, size_t len, FILE *f)
	{    
	if (f == NULL)
		return 0;

   if(fwrite(s, 1, len, f) != len)
	    return EOF;

	return 0;
	}
	
/* --------------------------------------------------------------------- */
int pqGetString(char *s, size_t len, FILE *f)
	{
	int c;

	if (f == NULL)
	return EOF;
  
	while (len-- && (c = getc(f)) != EOF && c)
		*s++ = c;
	*s = '\0';
   /* mjl: actually needs up to len+1 bytes, is this okay? XXX */

	return 0;
	}

/* --------------------------------------------------------------------- */
int pqPutString(const char *s, FILE *f)
	{
	if (f == NULL)
		return 0;
  
	if (fputs(s, f) == EOF)
		return EOF;

	fputc('\0', f); /* important to send an ending \0 since backend expects it */
	fflush(f);

	return 0;
	}

/* --------------------------------------------------------------------- */
int pqGetByte(FILE *f)
	{
	return getc(f);
	}
	
/* --------------------------------------------------------------------- */
int pqPutByte(int c, FILE *f)
	{
	if(!f)	return 0;
	
	return (putc(c, f) == c) ? 0 : EOF;
	}
	
/* --------------------------------------------------------------------- */

