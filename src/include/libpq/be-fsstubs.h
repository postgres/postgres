/*-------------------------------------------------------------------------
 *
 * be-fsstubs.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: be-fsstubs.h,v 1.11 2000/06/09 01:11:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BE_FSSTUBS_H
#define BE_FSSTUBS_H

#include "fmgr.h"

/*
 * LO functions available via pg_proc entries
 */
extern Datum lo_import(PG_FUNCTION_ARGS);
extern Datum lo_export(PG_FUNCTION_ARGS);

extern Datum lo_creat(PG_FUNCTION_ARGS);

extern Datum lo_open(PG_FUNCTION_ARGS);
extern Datum lo_close(PG_FUNCTION_ARGS);

extern Datum loread(PG_FUNCTION_ARGS);
extern Datum lowrite(PG_FUNCTION_ARGS);

extern Datum lo_lseek(PG_FUNCTION_ARGS);
extern Datum lo_tell(PG_FUNCTION_ARGS);
extern Datum lo_unlink(PG_FUNCTION_ARGS);

/*
 * These are not fmgr-callable, but are available to C code.
 * Probably these should have had the underscore-free names,
 * but too late now...
 */
extern int	lo_read(int fd, char *buf, int len);
extern int	lo_write(int fd, char *buf, int len);

/*
 * Cleanup LOs at xact commit/abort [ Pascal André <andre@via.ecp.fr> ]
 */
extern void lo_commit(bool isCommit);

#endif	 /* BE_FSSTUBS_H */
