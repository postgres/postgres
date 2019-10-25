/* src/interfaces/ecpg/pgtypeslib/pgtypeslib_extern.h */

#ifndef _ECPG_PGTYPESLIB_EXTERN_H
#define _ECPG_PGTYPESLIB_EXTERN_H

#include "pgtypes_error.h"

/* These are the constants that decide which printf() format we'll use in
 * order to get a string representation of the value */
#define PGTYPES_TYPE_NOTHING			0
#define PGTYPES_TYPE_STRING_MALLOCED		1
#define PGTYPES_TYPE_STRING_CONSTANT		2
#define PGTYPES_TYPE_CHAR			3
#define PGTYPES_TYPE_DOUBLE_NF			4	/* no fractional part */
#define PGTYPES_TYPE_INT64			5
#define PGTYPES_TYPE_UINT			6
#define PGTYPES_TYPE_UINT_2_LZ			7	/* 2 digits, pad with leading zero */
#define PGTYPES_TYPE_UINT_2_LS			8	/* 2 digits, pad with leading
											 * space */
#define PGTYPES_TYPE_UINT_3_LZ			9
#define PGTYPES_TYPE_UINT_4_LZ			10
#define PGTYPES_TYPE_UINT_LONG			11

#define PGTYPES_FMT_NUM_MAX_DIGITS		40

union un_fmt_comb
{
	char	   *str_val;
	unsigned int uint_val;
	char		char_val;
	unsigned long int luint_val;
	double		double_val;
	int64		int64_val;
};

int			pgtypes_fmt_replace(union un_fmt_comb, int, char **, int *);

char	   *pgtypes_alloc(long);
char	   *pgtypes_strdup(const char *);

#endif							/* _ECPG_PGTYPESLIB_EXTERN_H */
