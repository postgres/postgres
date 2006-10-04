#ifndef __HSTORE_H__
#define __HSTORE_H__

#include "postgres.h"
#include "funcapi.h"
#include "access/gist.h"
#include "access/itup.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"


typedef struct
{
	uint16		keylen;
	uint16		vallen;
	uint32
				valisnull:1,
				pos:31;
}	HEntry;


typedef struct
{
	int4		len;
	int4		size;
	char		data[1];
}	HStore;

#define HSHRDSIZE	(2*sizeof(int4))
#define CALCDATASIZE(x, lenstr) ( (x) * sizeof(HEntry) + HSHRDSIZE + (lenstr) )
#define ARRPTR(x)		( (HEntry*) ( (char*)(x) + HSHRDSIZE ) )
#define STRPTR(x)		( (char*)(x) + HSHRDSIZE + ( sizeof(HEntry) * ((HStore*)x)->size ) )


#define PG_GETARG_HS(x) ((HStore*)PG_DETOAST_DATUM(PG_GETARG_DATUM(x)))

typedef struct
{
	char	   *key;
	char	   *val;
	uint16		keylen;
	uint16		vallen;
	bool		isnull;
	bool		needfree;
}	Pairs;

int			comparePairs(const void *a, const void *b);
int			uniquePairs(Pairs * a, int4 l, int4 *buflen);

#endif
