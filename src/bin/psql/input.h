/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/input.h,v 1.20 2003/08/04 23:59:40 tgl Exp $
 */
#ifndef INPUT_H
#define INPUT_H

/*
 * If some other file needs to have access to readline/history, include this
 * file and save yourself all this work.
 *
 * USE_READLINE is the definite pointers regarding existence or not.
 */
#ifdef HAVE_LIBREADLINE
#define USE_READLINE 1
#if defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#elif defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#elif defined(HAVE_READLINE_H)
#include <readline.h>
#endif
#if defined(HAVE_READLINE_HISTORY_H)
#include <readline/history.h>
#elif defined(HAVE_EDITLINE_HISTORY_H)
#include <editline/history.h>
#elif defined(HAVE_HISTORY_H)
#include <history.h>
#endif
#endif


char	   *gets_interactive(const char *prompt);
char	   *gets_fromFile(FILE *source);

void		initializeInput(int flags);
bool		saveHistory(char *fname);

#endif   /* INPUT_H */
