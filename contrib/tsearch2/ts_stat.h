#ifndef __TXTIDX_STAT_H__
#define __TXTIDX_STAT_H__

#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"

typedef struct
{
	uint32		len;
	uint32		pos;
	uint32		ndoc;
	uint32		nentry;
}	StatEntry;

typedef struct
{
	int4		len;
	int4		size;
	char		data[1];
}	tsstat;

#define STATHDRSIZE (sizeof(int4)*2)
#define CALCSTATSIZE(x, lenstr) ( x * sizeof(StatEntry) + STATHDRSIZE + lenstr )
#define STATPTR(x)	( (StatEntry*) ( (char*)x + STATHDRSIZE ) )
#define STATSTRPTR(x)	( (char*)x + STATHDRSIZE + ( sizeof(StatEntry) * ((tsvector*)x)->size ) )
#define STATSTRSIZE(x)	( ((tsvector*)x)->len - STATHDRSIZE - ( sizeof(StatEntry) * ((tsvector*)x)->size ) )

#endif
