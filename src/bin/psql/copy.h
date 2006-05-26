/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/copy.h,v 1.19 2006/05/26 19:51:29 tgl Exp $
 */
#ifndef COPY_H
#define COPY_H

#include "libpq-fe.h"


/* handler for \copy */
bool		do_copy(const char *args);

/* lower level processors for copy in/out streams */

bool		handleCopyOut(PGconn *conn, FILE *copystream);
bool		handleCopyIn(PGconn *conn, FILE *copystream, bool isbinary);

#endif
