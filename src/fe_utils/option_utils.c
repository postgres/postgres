/*-------------------------------------------------------------------------
 *
 * Command line option processing facilities for frontend code
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/option_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "common/string.h"
#include "fe_utils/option_utils.h"

/*
 * Provide strictly harmonized handling of --help and --version
 * options.
 */
void
handle_help_version_opts(int argc, char *argv[],
						 const char *fixed_progname, help_handler hlp)
{
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			hlp(get_progname(argv[0]));
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			printf("%s (PostgreSQL) " PG_VERSION "\n", fixed_progname);
			exit(0);
		}
	}
}

/*
 * option_parse_int
 *
 * Parse integer value for an option.  If the parsing is successful, returns
 * true and stores the result in *result if that's given; if parsing fails,
 * returns false.
 */
bool
option_parse_int(const char *optarg, const char *optname,
				 int min_range, int max_range,
				 int *result)
{
	char	   *endptr;
	int			val;

	errno = 0;
	val = strtoint(optarg, &endptr, 10);

	/*
	 * Skip any trailing whitespace; if anything but whitespace remains before
	 * the terminating character, fail.
	 */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;

	if (*endptr != '\0')
	{
		pg_log_error("invalid value \"%s\" for option %s",
					 optarg, optname);
		return false;
	}

	if (errno == ERANGE || val < min_range || val > max_range)
	{
		pg_log_error("%s must be in range %d..%d",
					 optname, min_range, max_range);
		return false;
	}

	if (result)
		*result = val;
	return true;
}

/*
 * Provide strictly harmonized handling of the --sync-method option.
 */
bool
parse_sync_method(const char *optarg, DataDirSyncMethod *sync_method)
{
	if (strcmp(optarg, "fsync") == 0)
		*sync_method = DATA_DIR_SYNC_METHOD_FSYNC;
	else if (strcmp(optarg, "syncfs") == 0)
	{
#ifdef HAVE_SYNCFS
		*sync_method = DATA_DIR_SYNC_METHOD_SYNCFS;
#else
		pg_log_error("this build does not support sync method \"%s\"",
					 "syncfs");
		return false;
#endif
	}
	else
	{
		pg_log_error("unrecognized sync method: %s", optarg);
		return false;
	}

	return true;
}
