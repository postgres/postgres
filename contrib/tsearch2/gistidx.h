/* $PostgreSQL: pgsql/contrib/tsearch2/gistidx.h,v 1.7 2006/03/11 04:38:30 momjian Exp $ */

#ifndef __GISTIDX_H__
#define __GISTIDX_H__

/*
#define GISTIDX_DEBUG
*/

/*
 * signature defines
 */

#define SIGLENINT  63			/* >121 => key will toast, so it will not work
								 * !!! */
#define SIGLEN	( sizeof(int4) * SIGLENINT )
#define SIGLENBIT (SIGLEN * BITS_PER_BYTE)

typedef char BITVEC[SIGLEN];
typedef char *BITVECP;

#define LOOPBYTE(a) \
		for(i=0;i<SIGLEN;i++) {\
				a;\
		}

#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITS_PER_BYTE ) ) )
#define GETBITBYTE(x,i) ( ((char)(x)) >> (i) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITS_PER_BYTE )) & 0x01 )

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

#define ISARRKEY(x) ( ((GISTTYPE*)(x))->flag & ARRKEY )
#define ISSIGNKEY(x)	( ((GISTTYPE*)(x))->flag & SIGNKEY )
#define ISALLTRUE(x)	( ((GISTTYPE*)(x))->flag & ALLISTRUE )

#define GTHDRSIZE	( sizeof(int4) * 2	)
#define CALCGTSIZE(flag, len) ( GTHDRSIZE + ( ( (flag) & ARRKEY ) ? ((len)*sizeof(int4)) : (((flag) & ALLISTRUE) ? 0 : SIGLEN) ) )

#define GETSIGN(x)	( (BITVECP)( (char*)(x)+GTHDRSIZE ) )
#define GETARR(x)	( (int4*)( (char*)(x)+GTHDRSIZE ) )
#define ARRNELEM(x) ( ( ((GISTTYPE*)(x))->len - GTHDRSIZE )/sizeof(int4) )

#endif
