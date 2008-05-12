/*
 * $PostgreSQL: pgsql/contrib/hstore/hstore.h,v 1.6 2008/05/12 00:00:42 alvherre Exp $
 */
#ifndef __HSTORE_H__
#define __HSTORE_H__

#include "fmgr.h"


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
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int4		size;
	char		data[1];
}	HStore;

#define HSHRDSIZE	(VARHDRSZ + sizeof(int4))
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

#define HStoreContainsStrategyNumber	7
#define HStoreExistsStrategyNumber		9

#endif /* __HSTORE_H__ */
