#ifndef COPY_H
#define COPY_H

#include <c.h>
#include <stdio.h>
#include <libpq-fe.h>
#include "settings.h"

/* handler for \copy */
bool
			do_copy(const char *args, PsqlSettings *pset);


/* lower level processors for copy in/out streams */

bool
			handleCopyOut(PGconn *conn, FILE *copystream);

bool
			handleCopyIn(PGconn *conn, FILE *copystream, const char *prompt);

#endif
