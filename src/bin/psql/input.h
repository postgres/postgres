/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/input.h,v 1.6 2000/01/29 16:58:48 petere Exp $
 */
#ifndef INPUT_H
#define INPUT_H

#include <c.h>
#include <stdio.h>

/*
 * If some other file needs to have access to readline/history, include this
 * file and save yourself all this work.
 *
 * USE_READLINE and USE_HISTORY are the definite pointers regarding existence or not.
 */
#ifdef HAVE_LIBREADLINE
# ifdef HAVE_READLINE_H
#  include <readline.h>
#  define USE_READLINE 1
# elif defined(HAVE_READLINE_READLINE_H)
#  include <readline/readline.h>
#  define USE_READLINE 1
# endif
#endif

#if defined(HAVE_LIBHISTORY) || (defined(HAVE_LIBREADLINE) && defined(HAVE_HISTORY_IN_READLINE))
# ifdef HAVE_HISTORY_H
#  include <history.h>
#  define USE_HISTORY 1
# elif defined(HAVE_READLINE_HISTORY_H)
#  include <readline/history.h>
#  define USE_HISTORY 1
# endif
#endif

char * gets_interactive(const char *prompt);
char * gets_fromFile(FILE *source);

void initializeInput(int flags);
bool saveHistory(const char *fname);
void finishInput(void);

#endif /* INPUT_H */
