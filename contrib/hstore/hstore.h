/*
 * contrib/hstore/hstore.h
 */
#ifndef __HSTORE_H__
#define __HSTORE_H__

#include "fmgr.h"
#include "utils/array.h"


/*
 * HEntry: there is one of these for each key _and_ value in an hstore
 *
 * the position offset points to the _end_ so that we can get the length
 * by subtraction from the previous entry.  the ISFIRST flag lets us tell
 * whether there is a previous entry.
 */
typedef struct
{
	uint32		entry;
} HEntry;

#define HENTRY_ISFIRST 0x80000000
#define HENTRY_ISNULL  0x40000000
#define HENTRY_POSMASK 0x3FFFFFFF

/* note possible multiple evaluations, also access to prior array element */
#define HSE_ISFIRST(he_) (((he_).entry & HENTRY_ISFIRST) != 0)
#define HSE_ISNULL(he_) (((he_).entry & HENTRY_ISNULL) != 0)
#define HSE_ENDPOS(he_) ((he_).entry & HENTRY_POSMASK)
#define HSE_OFF(he_) (HSE_ISFIRST(he_) ? 0 : HSE_ENDPOS((&(he_))[-1]))
#define HSE_LEN(he_) (HSE_ISFIRST(he_)	\
					  ? HSE_ENDPOS(he_) \
					  : HSE_ENDPOS(he_) - HSE_ENDPOS((&(he_))[-1]))

/*
 * determined by the size of "endpos" (ie HENTRY_POSMASK), though this is a
 * bit academic since currently varlenas (and hence both the input and the
 * whole hstore) have the same limit
 */
#define HSTORE_MAX_KEY_LEN 0x3FFFFFFF
#define HSTORE_MAX_VALUE_LEN 0x3FFFFFFF

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint32		size_;			/* flags and number of items in hstore */
	/* array of HEntry follows */
} HStore;

/*
 * It's not possible to get more than 2^28 items into an hstore, so we reserve
 * the top few bits of the size field.  See hstore_compat.c for one reason
 * why.  Some bits are left for future use here.  MaxAllocSize makes the
 * practical count limit slightly more than 2^28 / 3, or INT_MAX / 24, the
 * limit for an hstore full of 4-byte keys and null values.  Therefore, we
 * don't explicitly check the format-imposed limit.
 */
#define HS_FLAG_NEWVERSION 0x80000000

#define HS_COUNT(hsp_) ((hsp_)->size_ & 0x0FFFFFFF)
#define HS_SETCOUNT(hsp_,c_) ((hsp_)->size_ = (c_) | HS_FLAG_NEWVERSION)


/*
 * "x" comes from an existing HS_COUNT() (as discussed, <= INT_MAX/24) or a
 * Pairs array length (due to MaxAllocSize, <= INT_MAX/40).  "lenstr" is no
 * more than INT_MAX, that extreme case arising in hstore_from_arrays().
 * Therefore, this calculation is limited to about INT_MAX / 5 + INT_MAX.
 */
#define HSHRDSIZE	(sizeof(HStore))
#define CALCDATASIZE(x, lenstr) ( (x) * 2 * sizeof(HEntry) + HSHRDSIZE + (lenstr) )

/* note multiple evaluations of x */
#define ARRPTR(x)		( (HEntry*) ( (HStore*)(x) + 1 ) )
#define STRPTR(x)		( (char*)(ARRPTR(x) + HS_COUNT((HStore*)(x)) * 2) )

/* note multiple/non evaluations */
#define HS_KEY(arr_,str_,i_) ((str_) + HSE_OFF((arr_)[2*(i_)]))
#define HS_VAL(arr_,str_,i_) ((str_) + HSE_OFF((arr_)[2*(i_)+1]))
#define HS_KEYLEN(arr_,i_) (HSE_LEN((arr_)[2*(i_)]))
#define HS_VALLEN(arr_,i_) (HSE_LEN((arr_)[2*(i_)+1]))
#define HS_VALISNULL(arr_,i_) (HSE_ISNULL((arr_)[2*(i_)+1]))

/*
 * currently, these following macros are the _only_ places that rely
 * on internal knowledge of HEntry. Everything else should be using
 * the above macros. Exception: the in-place upgrade in hstore_compat.c
 * messes with entries directly.
 */

/*
 * copy one key/value pair (which must be contiguous starting at
 * sptr_) into an under-construction hstore; dent_ is an HEntry*,
 * dbuf_ is the destination's string buffer, dptr_ is the current
 * position in the destination. lots of modification and multiple
 * evaluation here.
 */
#define HS_COPYITEM(dent_,dbuf_,dptr_,sptr_,klen_,vlen_,vnull_)			\
	do {																\
		memcpy((dptr_), (sptr_), (klen_)+(vlen_));						\
		(dptr_) += (klen_)+(vlen_);										\
		(dent_)++->entry = ((dptr_) - (dbuf_) - (vlen_)) & HENTRY_POSMASK; \
		(dent_)++->entry = ((((dptr_) - (dbuf_)) & HENTRY_POSMASK)		\
							 | ((vnull_) ? HENTRY_ISNULL : 0));			\
	} while(0)

/*
 * add one key/item pair, from a Pairs structure, into an
 * under-construction hstore
 */
#define HS_ADDITEM(dent_,dbuf_,dptr_,pair_)								\
	do {																\
		memcpy((dptr_), (pair_).key, (pair_).keylen);					\
		(dptr_) += (pair_).keylen;										\
		(dent_)++->entry = ((dptr_) - (dbuf_)) & HENTRY_POSMASK;		\
		if ((pair_).isnull)												\
			(dent_)++->entry = ((((dptr_) - (dbuf_)) & HENTRY_POSMASK)	\
								 | HENTRY_ISNULL);						\
		else															\
		{																\
			memcpy((dptr_), (pair_).val, (pair_).vallen);				\
			(dptr_) += (pair_).vallen;									\
			(dent_)++->entry = ((dptr_) - (dbuf_)) & HENTRY_POSMASK;	\
		}																\
	} while (0)

/* finalize a newly-constructed hstore */
#define HS_FINALIZE(hsp_,count_,buf_,ptr_)							\
	do {															\
		int buflen = (ptr_) - (buf_);								\
		if ((count_))												\
			ARRPTR(hsp_)[0].entry |= HENTRY_ISFIRST;				\
		if ((count_) != HS_COUNT((hsp_)))							\
		{															\
			HS_SETCOUNT((hsp_),(count_));							\
			memmove(STRPTR(hsp_), (buf_), buflen);					\
		}															\
		SET_VARSIZE((hsp_), CALCDATASIZE((count_), buflen));		\
	} while (0)

/* ensure the varlena size of an existing hstore is correct */
#define HS_FIXSIZE(hsp_,count_)											\
	do {																\
		int bl = (count_) ? HSE_ENDPOS(ARRPTR(hsp_)[2*(count_)-1]) : 0; \
		SET_VARSIZE((hsp_), CALCDATASIZE((count_),bl));					\
	} while (0)

/* DatumGetHStoreP includes support for reading old-format hstore values */
extern HStore *hstoreUpgrade(Datum orig);

#define DatumGetHStoreP(d) hstoreUpgrade(d)

#define PG_GETARG_HS(x) DatumGetHStoreP(PG_GETARG_DATUM(x))


/*
 * Pairs is a "decompressed" representation of one key/value pair.
 * The two strings are not necessarily null-terminated.
 */
typedef struct
{
	char	   *key;
	char	   *val;
	size_t		keylen;
	size_t		vallen;
	bool		isnull;			/* value is null? */
	bool		needfree;		/* need to pfree the value? */
} Pairs;

extern int	hstoreUniquePairs(Pairs *a, int32 l, int32 *buflen);
extern HStore *hstorePairs(Pairs *pairs, int32 pcount, int32 buflen);

extern size_t hstoreCheckKeyLen(size_t len);
extern size_t hstoreCheckValLen(size_t len);

extern int	hstoreFindKey(HStore *hs, int *lowbound, char *key, int keylen);
extern Pairs *hstoreArrayToPairs(ArrayType *a, int *npairs);

#define HStoreContainsStrategyNumber	7
#define HStoreExistsStrategyNumber		9
#define HStoreExistsAnyStrategyNumber	10
#define HStoreExistsAllStrategyNumber	11
#define HStoreOldContainsStrategyNumber 13		/* backwards compatibility */

/*
 * defining HSTORE_POLLUTE_NAMESPACE=0 will prevent use of old function names;
 * for now, we default to on for the benefit of people restoring old dumps
 */
#ifndef HSTORE_POLLUTE_NAMESPACE
#define HSTORE_POLLUTE_NAMESPACE 1
#endif

#if HSTORE_POLLUTE_NAMESPACE
#define HSTORE_POLLUTE(newname_,oldname_) \
	PG_FUNCTION_INFO_V1(oldname_);		  \
	Datum newname_(PG_FUNCTION_ARGS);	  \
	Datum oldname_(PG_FUNCTION_ARGS) { return newname_(fcinfo); } \
	extern int no_such_variable
#else
#define HSTORE_POLLUTE(newname_,oldname_) \
	extern int no_such_variable
#endif

#endif   /* __HSTORE_H__ */
