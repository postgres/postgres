#include <stdlib.h>
#include <stdio.h>

#include "postgres.h"
#include "libpq/pqcomm.h"

/* --------------------------------------------------------------------- */
/* Is the other way around than system ntoh/hton, so we roll our own
	here */
	
#if BYTE_ORDER == LITTLE_ENDIAN
#define ntoh_s(n) n
#define ntoh_l(n) n
#define hton_s(n) n
#define hton_l(n) n
#endif
#if BYTE_ORDER == BIG_ENDIAN
#define ntoh_s(n) (u_short)(((u_char *) &n)[0] << 8 | ((u_char *) &n)[1]);
#define ntoh_l(n) (u_long)(((u_char *)&n)[0] << 24 | ((u_char *)&n)[1] << 16 |\
      	             	   ((u_char *)&n)[2] << 8 | ((u_char *)&n)[3]);
#define hton_s(n) (ntoh_s(n))
#define hton_l(n) (ntoh_l(n))
#endif
#if BYTE_ORDER == PDP_ENDIAN
#endif
#ifndef ntoh_s
#error Please write byte order macros
#endif

/* --------------------------------------------------------------------- */
int pqPutShort(const int integer, FILE *f)
    {
    int retval = 0;
    u_short n;
		
    n = hton_s(integer);
    if(fwrite(&n, sizeof(u_short), 1, f) != 1)
    	retval = 1;
    
    return retval;
    }

/* --------------------------------------------------------------------- */
int pqPutLong(const int integer, FILE *f)
    {
    int retval = 0;
    u_long n;
		
    n = hton_l(integer);
    if(fwrite(&n, sizeof(u_long), 1, f) != 1)
    	retval = 1;
    
    return retval;
    }
    
/* --------------------------------------------------------------------- */
int pqGetShort(int *result, FILE *f)
    {
    int retval = 0;
    u_short n;

    if(fread(&n, sizeof(u_short), 1, f) != 1)
    	retval = 1;
			
    *result = ntoh_s(n);
    return retval;
    }

/* --------------------------------------------------------------------- */
int pqGetLong(int *result, FILE *f)
    {
    int retval = 0;
    u_long n;
		
    if(fread(&n, sizeof(u_long), 1, f) != 1)
    	retval = 1;
			
    *result = ntoh_l(n);
    return retval;
    }

/* --------------------------------------------------------------------- */
