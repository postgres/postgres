/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2015, PostgreSQL Global Development Group
 *
 * src/bin/psql/input.h
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
#if defined(HAVE_READLINE_HISTORY_H)
#include <readline/history.h>
#endif
#elif defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#if defined(HAVE_EDITLINE_HISTORY_H)
#include <editline/history.h>
#endif
#elif defined(HAVE_READLINE_H)
#include <readline.h>
#if defined(HAVE_HISTORY_H)
#include <history.h>
#endif
#endif   /* HAVE_READLINE_READLINE_H, etc */
#endif   /* HAVE_LIBREADLINE */

#include "pqexpbuffer.h"


char	   *gets_interactive(const char *prompt);
char	   *gets_fromFile(FILE *source);

void		initializeInput(int flags);

bool		printHistory(const char *fname, unsigned short int pager);

void		pg_append_history(const char *s, PQExpBuffer history_buf);
void		pg_send_history(PQExpBuffer history_buf);

#endif   /* INPUT_H */
