/* $PostgreSQL: pgsql/src/bin/psql/mbprint.h,v 1.8 2005/09/24 17:53:27 tgl Exp $ */
#ifndef MBPRINT_H
#define MBPRINT_H

#include "mb/pg_wchar.h"

extern char *mbvalidate(char *pwcs, int encoding);

extern int	pg_wcswidth(const char *pwcs, size_t len, int encoding);

#endif   /* MBPRINT_H */
