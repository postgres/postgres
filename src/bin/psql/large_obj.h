/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/large_obj.h,v 1.7 2000/01/29 16:58:49 petere Exp $
 */
#ifndef LARGE_OBJ_H
#define LARGE_OBJ_H

#include <c.h>

bool		do_lo_export(const char *loid_arg, const char *filename_arg);
bool		do_lo_import(const char *filename_arg, const char *comment_arg);
bool		do_lo_unlink(const char *loid_arg);
bool		do_lo_list(void);

#endif	 /* LARGE_OBJ_H */
