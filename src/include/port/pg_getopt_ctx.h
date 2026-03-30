/*
 * Re-entrant version of the standard getopt(3) function.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/port/pg_getopt_ctx.h
 */
#ifndef PG_GETOPT_CTX_H
#define PG_GETOPT_CTX_H

typedef struct
{
	int			nargc;
	char	   *const *nargv;
	const char *ostr;

	/*
	 * Caller can modify 'opterr' between pg_getopt_start() and the first call
	 * to pg_getopt_next().  Equivalent to the global variable of the same
	 * name in standard getopt(3).
	 */
	int			opterr;

	/*
	 * Output variables set by pg_getopt_next().  These are equivalent to the
	 * global variables with same names in standard getopt(3).
	 */
	char	   *optarg;
	int			optind;
	int			optopt;

	/* internal state */
	char	   *place;
} pg_getopt_ctx;

extern void pg_getopt_start(pg_getopt_ctx *ctx, int nargc, char *const *nargv, const char *ostr);
extern int	pg_getopt_next(pg_getopt_ctx *ctx);

#endif							/* PG_GETOPT_CTX_H */
