#ifndef __GISTIDX_H__
#define __GISTIDX_H__

/*
#define GISTIDX_DEBUG
*/

/*
 * signature defines
 */
#define BITBYTE 8
#define SIGLENINT  64			/* >121 => key will toast, so it will not
								 * work !!! */
#define SIGLEN	( sizeof(int4)*SIGLENINT )
#define SIGLENBIT (SIGLEN*BITBYTE)

typedef char BITVEC[SIGLEN];
typedef char *BITVECP;

#define LOOPBYTE(a) \
		for(i=0;i<SIGLEN;i++) {\
				a;\
		}
#define LOOPBIT(a) \
				for(i=0;i<SIGLENBIT;i++) {\
								a;\
				}

#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( ((char)(x)) >> i & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )

#define abs(a)			((a) <	(0) ? -(a) : (a))
#define min(a,b)			((a) <	(b) ? (a) : (b))
#define HASHVAL(val) (((unsigned int)(val)) % SIGLENBIT)
#define HASH(sign, val) SETBIT((sign), HASHVAL(val))


/*
 * type of index key
 */
typedef struct
{
	int4		len;
	int4		flag;
	char		data[1];
}	GISTTYPE;

#define ARRKEY		0x01
#define SIGNKEY		0x02
#define ALLISTRUE	0x04

#define ISARRKEY(x) ( ((GISTTYPE*)x)->flag & ARRKEY )
#define ISSIGNKEY(x)	( ((GISTTYPE*)x)->flag & SIGNKEY )
#define ISALLTRUE(x)	( ((GISTTYPE*)x)->flag & ALLISTRUE )

#define GTHDRSIZE	( sizeof(int4)*2  )
#define CALCGTSIZE(flag, len) ( GTHDRSIZE + ( ( (flag) & ARRKEY ) ? ((len)*sizeof(int4)) : (((flag) & ALLISTRUE) ? 0 : SIGLEN) ) )

#define GETSIGN(x)	( (BITVECP)( (char*)x+GTHDRSIZE ) )
#define GETARR(x)	( (int4*)( (char*)x+GTHDRSIZE ) )
#define ARRNELEM(x) ( ( ((GISTTYPE*)x)->len - GTHDRSIZE )/sizeof(int4) )

#endif
