#ifndef __PGTYPES_COMMON_H__
#define __PGTYPES_COMMON_H__

#include "pgtypes_error.h"

/* These are the constants that decide which printf() format we'll use in
 * order to get a string representation of the value */
#define PGTYPES_REPLACE_NOTHING			0
#define PGTYPES_REPLACE_STRING_MALLOCED		1
#define PGTYPES_REPLACE_STRING_CONSTANT		2
#define PGTYPES_REPLACE_CHAR			3
#define PGTYPES_REPLACE_DOUBLE_NF		4   /* no fractional part */
#define PGTYPES_REPLACE_INT64			5
#define PGTYPES_REPLACE_UINT			6
#define PGTYPES_REPLACE_UINT_2_LZ		7   /* 2 digits, pad with leading zero */
#define PGTYPES_REPLACE_UINT_2_LS		8   /* 2 digits, pad with leading space */
#define PGTYPES_REPLACE_UINT_3_LZ		9
#define PGTYPES_REPLACE_UINT_4_LZ		10

#define PGTYPES_FMT_NUM_MAX_DIGITS		40

union un_fmt_replace {
	char*			replace_str;
	unsigned int		replace_uint;
	char			replace_char;
	unsigned long int	replace_luint;
	double			replace_double;
#ifdef HAVE_INT64_TIMESTAMP
	int64			replace_int64;
#endif
};

int pgtypes_fmt_replace(union un_fmt_replace, int, char**, int*);

char *pgtypes_alloc(long);
char *pgtypes_strdup(char *);

#ifndef bool
#define bool char
#endif   /* ndef bool */

#ifndef FALSE
#define FALSE   0
#endif   /* FALSE */

#ifndef TRUE
#define TRUE       1
#endif  /* TRUE */

#endif /* __PGTYPES_COMMON_H__ */

