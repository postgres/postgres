/*-------------------------------------------------------------------------
 *
 * logging.c
 *	 logging functions
 *
 *	Copyright (c) 2010-2019, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <time.h>

#include "pg_rewind.h"
#include "logging.h"

#include "pgtime.h"

/* Progress counters */
uint64		fetch_size;
uint64		fetch_done;

static pg_time_t last_progress_report = 0;


/*
 * Print a progress report based on the global variables.
 *
 * Progress report is written at maximum once per second, unless the
 * force parameter is set to true.
 */
void
progress_report(bool force)
{
	int			percent;
	char		fetch_done_str[32];
	char		fetch_size_str[32];
	pg_time_t	now;

	if (!showprogress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force)
		return;					/* Max once per second */

	last_progress_report = now;
	percent = fetch_size ? (int) ((fetch_done) * 100 / fetch_size) : 0;

	/*
	 * Avoid overflowing past 100% or the full size. This may make the total
	 * size number change as we approach the end of the backup (the estimate
	 * will always be wrong if WAL is included), but that's better than having
	 * the done column be bigger than the total.
	 */
	if (percent > 100)
		percent = 100;
	if (fetch_done > fetch_size)
		fetch_size = fetch_done;

	/*
	 * Separate step to keep platform-dependent format code out of
	 * translatable strings.  And we only test for INT64_FORMAT availability
	 * in snprintf, not fprintf.
	 */
	snprintf(fetch_done_str, sizeof(fetch_done_str), INT64_FORMAT,
			 fetch_done / 1024);
	snprintf(fetch_size_str, sizeof(fetch_size_str), INT64_FORMAT,
			 fetch_size / 1024);

	fprintf(stderr, _("%*s/%s kB (%d%%) copied"),
		   (int) strlen(fetch_size_str), fetch_done_str, fetch_size_str,
		   percent);
	if (isatty(fileno(stderr)))
		fprintf(stderr, "\r");
	else
		fprintf(stderr, "\n");
}
