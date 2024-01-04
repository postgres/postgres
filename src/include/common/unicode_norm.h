/*-------------------------------------------------------------------------
 *
 * unicode_norm.h
 *	  Routines for normalizing Unicode strings
 *
 * These definitions are used by both frontend and backend code.
 *
 * Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_norm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_NORM_H
#define UNICODE_NORM_H

#include "mb/pg_wchar.h"

typedef enum
{
	UNICODE_NFC = 0,
	UNICODE_NFD = 1,
	UNICODE_NFKC = 2,
	UNICODE_NFKD = 3,
} UnicodeNormalizationForm;

/* see UAX #15 */
typedef enum
{
	UNICODE_NORM_QC_NO = 0,
	UNICODE_NORM_QC_YES = 1,
	UNICODE_NORM_QC_MAYBE = -1,
} UnicodeNormalizationQC;

extern pg_wchar *unicode_normalize(UnicodeNormalizationForm form, const pg_wchar *input);

extern UnicodeNormalizationQC unicode_is_normalized_quickcheck(UnicodeNormalizationForm form, const pg_wchar *input);

#endif							/* UNICODE_NORM_H */
