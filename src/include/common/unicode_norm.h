/*-------------------------------------------------------------------------
 *
 * unicode_norm.h
 *	  Routines for normalizing Unicode strings
 *
 * These definitions are used by both frontend and backend code.
 *
 * Copyright (c) 2017-2019, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_norm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_NORM_H
#define UNICODE_NORM_H

#include "mb/pg_wchar.h"

extern pg_wchar *unicode_normalize_kc(const pg_wchar *input);

#endif							/* UNICODE_NORM_H */
