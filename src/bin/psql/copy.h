/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/copy.h,v 1.10 2001/10/25 05:49:53 momjian Exp $
 */
#ifndef COPY_H
#define COPY_H

#include "libpq-fe.h"

extern bool copy_in_state;

/* handler for \copy */
bool		do_copy(const char *args);

/* lower level processors for copy in/out streams */

bool		handleCopyOut(PGconn *conn, FILE *copystream);
bool		handleCopyIn(PGconn *conn, FILE *copystream, const char *prompt);
#endif
