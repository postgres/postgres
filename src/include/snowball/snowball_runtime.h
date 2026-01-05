/*-------------------------------------------------------------------------
 *
 * snowball_runtime.h
 *		Replacement header file for Snowball stemmer modules
 *
 * The Snowball stemmer modules do #include "snowball_runtime.h", and think
 * they are including snowball/libstemmer/snowball_runtime.h.  We adjust
 * the CPPFLAGS so that this file is found instead, and thereby we can modify
 * the headers they see.  The main point here is to ensure that pg_config.h
 * is included before any system headers such as <stdio.h>; without that,
 * we have portability issues on some platforms due to variation in
 * largefile options across different modules in the backend.
 *
 * NOTE: this file should not be included into any non-snowball sources!
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/snowball/snowball_runtime.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_SNOWBALL_RUNTIME_H
#define PG_SNOWBALL_RUNTIME_H

/*
 * It's against Postgres coding conventions to include postgres.h in a
 * header file, but we allow the violation here because the alternative is
 * to modify the machine-generated .c files provided by the Snowball project.
 */
#include "postgres.h"

/* Some platforms define MAXINT and/or MININT, causing conflicts */
#ifdef MAXINT
#undef MAXINT
#endif
#ifdef MININT
#undef MININT
#endif

/* Now we can include the original Snowball snowball_runtime.h */
#include "snowball/libstemmer/snowball_runtime.h"

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

#endif							/* PG_SNOWBALL_RUNTIME_H */
