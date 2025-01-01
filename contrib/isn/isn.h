/*-------------------------------------------------------------------------
 *
 * isn.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * Author:	German Mendez Bravo (Kronuz)
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/isn/isn.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef ISN_H
#define ISN_H

#include "fmgr.h"

#undef ISN_DEBUG
#define ISN_WEAK_MODE

/*
 *	uint64 is the internal storage format for ISNs.
 */
typedef uint64 ean13;

#define EAN13_FORMAT UINT64_FORMAT

#define PG_GETARG_EAN13(n) PG_GETARG_INT64(n)
#define PG_RETURN_EAN13(x) PG_RETURN_INT64(x)

extern void initialize(void);

#endif							/* ISN_H */
