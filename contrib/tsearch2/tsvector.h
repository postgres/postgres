#ifndef __TXTIDX_H__
#define __TXTIDX_H__

/*
#define TXTIDX_DEBUG
*/

#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"

typedef struct
{
	uint32
				haspos:1,
				len:11,			/* MAX 2Kb */
				pos:20;			/* MAX 1Mb */
}	WordEntry;

#define MAXSTRLEN ( 1<<11 )
#define MAXSTRPOS ( 1<<20 )

/*
Equivalent to
typedef struct
{
	   uint16
								weight:2,
								pos:14;
}	WordEntryPos;

*/

typedef uint16 WordEntryPos;

#define  WEP_GETWEIGHT(x)	( (x) >> 14 )
#define  WEP_GETPOS(x)		( (x) & 0x3fff )

#define  WEP_SETWEIGHT(x,v)  (x) = ( (v) << 14 ) | ( (x) & 0x3fff )
#define  WEP_SETPOS(x,v)	(x) = ( (x) & 0xc000 ) | ( (v) & 0x3fff )


#define MAXENTRYPOS (1<<14)
#define MAXNUMPOS	256
#define LIMITPOS(x) ( ( (x) >= MAXENTRYPOS ) ? (MAXENTRYPOS-1) : (x) )

/*
 * Structure of tsvector datatype:
 * 1) int4	len - varlena's length
 * 2) int4		size - number of lexemes or WordEntry array, which is the same
 * 3) Array of WordEntry - sorted array, comparison based on word's length
 *						and strncmp(). WordEntry->pos points number of
 *						bytes from end of WordEntry array to start of
 *						corresponding lexeme.
 * 4) Lexeme's storage:
 *	  SHORTALIGNED(lexeme) and position information if it exists
 *	  Position information: first int2 - is a number of positions and it
 *	  follows array of WordEntryPos
 */

typedef struct
{
	int4		len;
	int4		size;
	char		data[1];
}	tsvector;

#define DATAHDRSIZE (sizeof(int4) * 2)
#define CALCDATASIZE(x, lenstr) ( (x) * sizeof(WordEntry) + DATAHDRSIZE + (lenstr) )
#define ARRPTR(x)	( (WordEntry*) ( (char*)(x) + DATAHDRSIZE ) )
#define STRPTR(x)	( (char*)(x) + DATAHDRSIZE + ( sizeof(WordEntry) * ((tsvector*)(x))->size ) )
#define STRSIZE(x)	( ((tsvector*)(x))->len - DATAHDRSIZE - ( sizeof(WordEntry) * ((tsvector*)(x))->size ) )
#define _POSDATAPTR(x,e)	(STRPTR(x)+((WordEntry*)(e))->pos+SHORTALIGN(((WordEntry*)(e))->len))
#define POSDATALEN(x,e) ( ( ((WordEntry*)(e))->haspos ) ? (*(uint16*)_POSDATAPTR(x,e)) : 0 )
#define POSDATAPTR(x,e) ( (WordEntryPos*)( _POSDATAPTR(x,e)+sizeof(uint16) ) )


typedef struct
{
	WordEntry	entry;
	WordEntryPos *pos;
}	WordEntryIN;

typedef struct
{
	char	   *prsbuf;
	char	   *word;
	char	   *curpos;
	int4		len;
	int4		state;
	int4		alen;
	WordEntryPos *pos;
	bool		oprisdelim;
}	TI_IN_STATE;

int4		gettoken_tsvector(TI_IN_STATE * state);

#endif
