/*
 * Portions Copyright (c) 1987, 1993, 1994
 * The Regents of the University of California.  All rights reserved.
 *
 * Portions Copyright (c) 2003
 * PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/include/getopt_long.h,v 1.2 2003/08/04 00:43:29 momjian Exp $
 */

/* These are picked up from the system's getopt() facility. */
extern int	opterr;
extern int	optind;
extern int	optopt;
extern char *optarg;

/* Some systems have this, otherwise you need to define it somewhere. */
extern int	optreset;

struct option
{
	const char *name;
	int			has_arg;
	int		   *flag;
	int			val;
};

#define no_argument 0
#define required_argument 1

int getopt_long(int argc, char *const argv[],
			const char *optstring,
			const struct option * longopts, int *longindex);
