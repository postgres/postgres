
/* -----------------------------------------------------------------------
 * formatting.h
 *
 * $Id: formatting.h,v 1.5 2000/06/09 01:11:15 tgl Exp $
 *
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL, Inc
 *
 *	 The PostgreSQL routines for a DateTime/int/float/numeric formatting,
 *	 inspire with Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() routines.
 *
 *	 Karel Zak - Zakkr
 *
 * -----------------------------------------------------------------------
 */

#ifndef _FORMATTING_H_
#define _FORMATTING_H_

#include "fmgr.h"


extern Datum timestamp_to_char(PG_FUNCTION_ARGS);
extern Datum to_timestamp(PG_FUNCTION_ARGS);
extern Datum to_date(PG_FUNCTION_ARGS);
extern Numeric numeric_to_number(text *value, text *fmt);
extern text *numeric_to_char(Numeric value, text *fmt);
extern text *int4_to_char(int32 value, text *fmt);
extern text *int8_to_char(int64 *value, text *fmt);
extern text *float4_to_char(float32 value, text *fmt);
extern text *float8_to_char(float64 value, text *fmt);

#endif
