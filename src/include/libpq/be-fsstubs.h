/*-------------------------------------------------------------------------
 *
 * be-fsstubs.h
 *
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/be-fsstubs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BE_FSSTUBS_H
#define BE_FSSTUBS_H

#include "fmgr.h"

/*
 * LO functions available via pg_proc entries
 */
extern Datum be_lo_import(PG_FUNCTION_ARGS);
extern Datum be_lo_import_with_oid(PG_FUNCTION_ARGS);
extern Datum be_lo_export(PG_FUNCTION_ARGS);

extern Datum be_lo_creat(PG_FUNCTION_ARGS);
extern Datum be_lo_create(PG_FUNCTION_ARGS);
extern Datum be_lo_from_bytea(PG_FUNCTION_ARGS);

extern Datum be_lo_open(PG_FUNCTION_ARGS);
extern Datum be_lo_close(PG_FUNCTION_ARGS);

extern Datum be_loread(PG_FUNCTION_ARGS);
extern Datum be_lowrite(PG_FUNCTION_ARGS);

extern Datum be_lo_get(PG_FUNCTION_ARGS);
extern Datum be_lo_get_fragment(PG_FUNCTION_ARGS);
extern Datum be_lo_put(PG_FUNCTION_ARGS);

extern Datum be_lo_lseek(PG_FUNCTION_ARGS);
extern Datum be_lo_tell(PG_FUNCTION_ARGS);
extern Datum be_lo_lseek64(PG_FUNCTION_ARGS);
extern Datum be_lo_tell64(PG_FUNCTION_ARGS);
extern Datum be_lo_unlink(PG_FUNCTION_ARGS);
extern Datum be_lo_truncate(PG_FUNCTION_ARGS);
extern Datum be_lo_truncate64(PG_FUNCTION_ARGS);

/*
 * compatibility option for access control
 */
extern bool lo_compat_privileges;

/*
 * These are not fmgr-callable, but are available to C code.
 * Probably these should have had the underscore-free names,
 * but too late now...
 */
extern int	lo_read(int fd, char *buf, int len);
extern int	lo_write(int fd, const char *buf, int len);

/*
 * Cleanup LOs at xact commit/abort
 */
extern void AtEOXact_LargeObject(bool isCommit);
extern void AtEOSubXact_LargeObject(bool isCommit, SubTransactionId mySubid,
						SubTransactionId parentSubid);

#endif   /* BE_FSSTUBS_H */
