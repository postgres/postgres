/*-------------------------------------------------------------------------
 *
 * rlstubs.h
 *    stub routines when compiled without readline and history libraries
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/Attic/rlstubs.h,v 1.1 1996/11/11 05:55:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
extern char *readline(char *prompt);

extern int write_history(char *dum);

extern int using_history(void);

extern int add_history(char *dum);
