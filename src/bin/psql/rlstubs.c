/*-------------------------------------------------------------------------
 *
 * rlstubs.c--
 *    stub routines when compiled without readline and history libraries
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/psql/Attic/rlstubs.c,v 1.2 1996/07/30 07:47:58 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>

char *
readline(char *prompt)
{
    static char buf[500];

    printf("%s", prompt);
    return fgets(buf, 500, stdin);
}

int
write_history()
{
    return 0;
}

int
using_history()
{
    return 0;
}

int
add_history()
{
    return 0;
}
