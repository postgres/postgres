/*-------------------------------------------------------------------------
 *
 * header.h
 *		Replacement header file for Snowball stemmer modules
 *
 * The Snowball stemmer modules do #include "header.h", and think they
 * are including snowball/libstemmer/header.h.  We adjust the CPPFLAGS
 * so that this file is found instead, and thereby we can modify the
 * headers they see.  The main point here is to ensure that pg_config.h
 * is included before any system headers such as <stdio.h>; without that,
 * we have portability issues on some platforms due to variation in
 * largefile options across different modules in the backend.
 *
 * NOTE: this file should not be included into any non-snowball sources!
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 * src/include/snowball/header.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNOWBALL_HEADR_H
#define SNOWBALL_HEADR_H

#include "postgres.h"

/* Some platforms define MAXINT and/or MININT, causing conflicts */
#ifdef MAXINT
#undef MAXINT
#endif
#ifdef MININT
#undef MININT
#endif

/* Now we can include the original Snowball header.h */
#include "snowball/libstemmer/header.h" /* pgrminclude ignore */

/*
 * Redefine standard memory allocation interface to pgsql's one.
 * This allows us to control where the Snowball code allocates stuff.
 */
#ifdef malloc
#undef malloc
#endif
#define malloc(a)		palloc(a)

#ifdef calloc
#undef calloc
#endif
#define calloc(a,b)		palloc0((a) * (b))

#ifdef realloc
#undef realloc
#endif
#define realloc(a,b)	repalloc(a,b)

#ifdef free
#undef free
#endif
#define free(a)			pfree(a)

#endif   /* SNOWBALL_HEADR_H */
