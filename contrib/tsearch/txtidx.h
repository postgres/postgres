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
	uint16		len;
	uint16		pos;
}	WordEntry;

typedef struct
{
	int4		len;
	int4		size;
	char		data[1];
}	txtidx;

#define DATAHDRSIZE (sizeof(int4)*2)
#define CALCDATASIZE(x, lenstr) ( x * sizeof(WordEntry) + DATAHDRSIZE + lenstr )
#define ARRPTR(x)	( (WordEntry*) ( (char*)x + DATAHDRSIZE ) )
#define STRPTR(x)	( (char*)x + DATAHDRSIZE + ( sizeof(WordEntry) * ((txtidx*)x)->size ) )
#define STRSIZE(x)	( ((txtidx*)x)->len - DATAHDRSIZE - ( sizeof(WordEntry) * ((txtidx*)x)->size ) )

typedef struct
{
	char	   *prsbuf;
	char	   *word;
	char	   *curpos;
	int4		len;
	int4		state;
	bool		oprisdelim;
}	TI_IN_STATE;

int4		gettoken_txtidx(TI_IN_STATE * state);

#endif
