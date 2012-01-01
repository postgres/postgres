/*-------------------------------------------------------------------------
 *
 * pg_collation_fn.h
 *	 prototypes for functions in catalog/pg_collation.c
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_collation_fn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_COLLATION_FN_H
#define PG_COLLATION_FN_H

extern Oid CollationCreate(const char *collname, Oid collnamespace,
				Oid collowner,
				int32 collencoding,
				const char *collcollate, const char *collctype);
extern void RemoveCollationById(Oid collationOid);

#endif   /* PG_COLLATION_FN_H */
