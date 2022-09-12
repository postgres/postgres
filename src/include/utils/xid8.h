/*-------------------------------------------------------------------------
 *
 * xid8.h
 *	  Header file for the "xid8" ADT.
 *
 * Copyright (c) 2020-2022, PostgreSQL Global Development Group
 *
 * src/include/utils/xid8.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XID8_H
#define XID8_H

#include "access/transam.h"

#define DatumGetFullTransactionId(X) (FullTransactionIdFromU64(DatumGetUInt64(X)))
#define FullTransactionIdGetDatum(X) (UInt64GetDatum(U64FromFullTransactionId(X)))
#define PG_GETARG_FULLTRANSACTIONID(X) DatumGetFullTransactionId(PG_GETARG_DATUM(X))
#define PG_RETURN_FULLTRANSACTIONID(X) return FullTransactionIdGetDatum(X)

#endif							/* XID8_H */
