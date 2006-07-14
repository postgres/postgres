#ifndef __TRGM_H__
#define __TRGM_H__

#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"

/* options */
#define LPADDING		2
#define RPADDING		1
#define KEEPONLYALNUM
#define IGNORECASE
#define DIVUNION


typedef char trgm[3];

#define CMPCHAR(a,b) ( ((a)==(b)) ? 0 : ( ((a)<(b)) ? -1 : 1 ) )
#define CMPPCHAR(a,b,i)  CMPCHAR( *(((char*)(a))+i), *(((char*)(b))+i) )
#define CMPTRGM(a,b) ( CMPPCHAR(a,b,0) ? CMPPCHAR(a,b,0) : ( CMPPCHAR(a,b,1) ? CMPPCHAR(a,b,1) : CMPPCHAR(a,b,2) ) )

#define CPTRGM(a,b) do {				\
	*(((char*)(a))+0) = *(((char*)(b))+0);	\
	*(((char*)(a))+1) = *(((char*)(b))+1);	\
	*(((char*)(a))+2) = *(((char*)(b))+2);	\
} while(0);


typedef struct
{
	int4		len;
	uint8		flag;
	char		data[1];
}	TRGM;

#define TRGMHRDSIZE		  (sizeof(int4)+sizeof(uint8))

/* gist */
#define BITBYTE 8
#define SIGLENINT  3			/* >122 => key will toast, so very slow!!! */
#define SIGLEN	( sizeof(int)*SIGLENINT )

#define SIGLENBIT (SIGLEN*BITBYTE - 1)	/* see makesign */

typedef char BITVEC[SIGLEN];
typedef char *BITVECP;

#define LOOPBYTE(a) \
				for(i=0;i<SIGLEN;i++) {\
								a;\
				}

#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( ((char)(x)) >> i & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )

#define HASHVAL(val) (((unsigned int)(val)) % SIGLENBIT)
#define HASH(sign, val) SETBIT((sign), HASHVAL(val))

#define ARRKEY			0x01
#define SIGNKEY			0x02
#define ALLISTRUE		0x04

#define ISARRKEY(x) ( ((TRGM*)x)->flag & ARRKEY )
#define ISSIGNKEY(x)	( ((TRGM*)x)->flag & SIGNKEY )
#define ISALLTRUE(x)	( ((TRGM*)x)->flag & ALLISTRUE )

#define CALCGTSIZE(flag, len) ( TRGMHRDSIZE + ( ( (flag) & ARRKEY ) ? ((len)*sizeof(trgm)) : (((flag) & ALLISTRUE) ? 0 : SIGLEN) ) )
#define GETSIGN(x)		( (BITVECP)( (char*)x+TRGMHRDSIZE ) )
#define GETARR(x)		( (trgm*)( (char*)x+TRGMHRDSIZE ) )
#define ARRNELEM(x) ( ( ((TRGM*)x)->len - TRGMHRDSIZE )/sizeof(trgm) )

extern float4 trgm_limit;
TRGM	   *generate_trgm(char *str, int slen);
float4		cnt_sml(TRGM * trg1, TRGM * trg2);

#endif
