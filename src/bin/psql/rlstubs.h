/*-------------------------------------------------------------------------
 *
 * rlstubs.h
 *    stub routines when compiled without readline and history libraries
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/Attic/rlstubs.h,v 1.2 1996/11/11 14:55:49 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
extern char *readline(const char *prompt);
extern int write_history(const char *dum);
extern int using_history(void);
extern int add_history(const char *dum);
