#ifndef INPUT_H
#define INPUT_H

#include <config.h>
#include <c.h>
#include <stdio.h>
#include "settings.h"


/* If some other file needs to have access to readline/history, include this
 * file and save yourself all this work.
 *
 * USE_READLINE and USE_HISTORY are the definite pointers regarding existence or not.
 */
#ifdef HAVE_LIBREADLINE
#ifdef HAVE_READLINE_H
#include <readline.h>
#define USE_READLINE 1
#else
#if defined(HAVE_READLINE_READLINE_H)
#include <readline/readline.h>
#define USE_READLINE 1
#endif
#endif
#endif

#if defined(HAVE_LIBHISTORY) || (defined(HAVE_LIBREADLINE) && defined(HAVE_HISTORY_IN_READLINE))
#if defined(HAVE_HISTORY_H)
#include <history.h>
#define USE_HISTORY 1
#else
#if defined(HAVE_READLINE_HISTORY_H)
#include <readline/history.h>
#define USE_HISTORY 1
#endif
#endif
#endif


char *
			gets_interactive(const char *prompt);

char *
			gets_fromFile(FILE *source);


void
			initializeInput(int flags);

bool
			saveHistory(const char *fname);

void
			finishInput(void);

#endif
