/*-------------------------------------------------------------------------
 *
 * isn.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * Copyright (c) 2004-2006, Germán Méndez Bravo (Kronuz)
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/isn/isn.h,v 1.7 2009/01/01 17:23:32 momjian Exp $
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

extern Datum isn_out(PG_FUNCTION_ARGS);
extern Datum ean13_out(PG_FUNCTION_ARGS);
extern Datum ean13_in(PG_FUNCTION_ARGS);
extern Datum isbn_in(PG_FUNCTION_ARGS);
extern Datum ismn_in(PG_FUNCTION_ARGS);
extern Datum issn_in(PG_FUNCTION_ARGS);
extern Datum upc_in(PG_FUNCTION_ARGS);

extern Datum isbn_cast_from_ean13(PG_FUNCTION_ARGS);
extern Datum ismn_cast_from_ean13(PG_FUNCTION_ARGS);
extern Datum issn_cast_from_ean13(PG_FUNCTION_ARGS);
extern Datum upc_cast_from_ean13(PG_FUNCTION_ARGS);

extern Datum is_valid(PG_FUNCTION_ARGS);
extern Datum make_valid(PG_FUNCTION_ARGS);

extern Datum accept_weak_input(PG_FUNCTION_ARGS);
extern Datum weak_input_status(PG_FUNCTION_ARGS);

extern void initialize(void);

#endif   /* ISN_H */
