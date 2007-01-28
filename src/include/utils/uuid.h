/*-------------------------------------------------------------------------
 *
 * uuid.h
 *	  Header file for the "uuid" data type.
 *
 * Copyright (c) 2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/utils/uuid.h,v 1.1 2007/01/28 16:16:54 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */ 
#ifndef UUID_H
#define UUID_H

/* guid size in bytes */
#define UUID_LEN 16

/* opaque struct; defined in uuid.c */
typedef struct uuid_t uuid_t;

/* fmgr interface macros */
#define UUIDPGetDatum(X)		PointerGetDatum(X)
#define PG_RETURN_UUID_P(X)		return UUIDPGetDatum(X)
#define DatumGetUUIDP(X)		((uuid_t *) DatumGetPointer(X))
#define PG_GETARG_UUID_P(X)		DatumGetUUIDP(PG_GETARG_DATUM(X))

#endif   /* UUID_H */
