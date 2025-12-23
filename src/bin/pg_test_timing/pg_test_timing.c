/*
 *	pg_test_timing.c
 *		tests overhead of timing calls and their monotonicity:	that
 *		they always move forward
 */

#include "postgres_fe.h"

#include <limits.h>

#include "getopt_long.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"

static const char *progname;

static unsigned int test_duration = 3;
static double max_rprct = 99.99;

/* record duration in powers of 2 nanoseconds */
static long long int histogram[32];

/* record counts of first 10K durations directly */
#define NUM_DIRECT 10000
static long long int direct_histogram[NUM_DIRECT];

/* separately record highest observed duration */
static int32 largest_diff;
static long long int largest_diff_count;


static void handle_args(int argc, char *argv[]);
static uint64 test_timing(unsigned int duration);
static void output(uint64 loop_count);

int
main(int argc, char *argv[])
{
	uint64		loop_count;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_test_timing"));
	progname = get_progname(argv[0]);

	handle_args(argc, argv);

	loop_count = test_timing(test_duration);

	output(loop_count);

	return 0;
}

static void
handle_args(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"duration", required_argument, NULL, 'd'},
		{"cutoff", required_argument, NULL, 'c'},
		{NULL, 0, NULL, 0}
	};

	int			option;			/* Command line option */
	int			optindex = 0;	/* used by getopt_long */
	unsigned long optval;		/* used for option parsing */
	char	   *endptr;

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			printf(_("Usage: %s [-d DURATION] [-c CUTOFF]\n"), progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_test_timing (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((option = getopt_long(argc, argv, "d:c:",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'd':
				errno = 0;
				optval = strtoul(optarg, &endptr, 10);

				if (endptr == optarg || *endptr != '\0' ||
					errno != 0 || optval != (unsigned int) optval)
				{
					fprintf(stderr, _("%s: invalid argument for option %s\n"),
							progname, "--duration");
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}

				test_duration = (unsigned int) optval;
				if (test_duration == 0)
				{
					fprintf(stderr, _("%s: %s must be in range %u..%u\n"),
							progname, "--duration", 1, UINT_MAX);
					exit(1);
				}
				break;

			case 'c':
				errno = 0;
				max_rprct = strtod(optarg, &endptr);

				if (endptr == optarg || *endptr != '\0' || errno != 0)
				{
					fprintf(stderr, _("%s: invalid argument for option %s\n"),
							progname, "--cutoff");
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}

				if (max_rprct < 0 || max_rprct > 100)
				{
					fprintf(stderr, _("%s: %s must be in range %u..%u\n"),
							progname, "--cutoff", 0, 100);
					exit(1);
				}
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
				break;
		}
	}

	if (argc > optind)
	{
		fprintf(stderr,
				_("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	printf(ngettext("Testing timing overhead for %u second.\n",
					"Testing timing overhead for %u seconds.\n",
					test_duration),
		   test_duration);
}

static uint64
test_timing(unsigned int duration)
{
	uint64		total_time;
	int64		time_elapsed = 0;
	uint64		loop_count = 0;
	uint64		prev,
				cur;
	instr_time	start_time,
				end_time,
				temp;

	/*
	 * Pre-zero the statistics data structures.  They're already zero by
	 * default, but this helps bring them into processor cache and avoid
	 * possible timing glitches due to COW behavior.
	 */
	memset(direct_histogram, 0, sizeof(direct_histogram));
	memset(histogram, 0, sizeof(histogram));
	largest_diff = 0;
	largest_diff_count = 0;

	total_time = duration > 0 ? duration * INT64CONST(1000000000) : 0;

	INSTR_TIME_SET_CURRENT(start_time);
	cur = INSTR_TIME_GET_NANOSEC(start_time);

	while (time_elapsed < total_time)
	{
		int32		diff,
					bits;

		prev = cur;
		INSTR_TIME_SET_CURRENT(temp);
		cur = INSTR_TIME_GET_NANOSEC(temp);
		diff = cur - prev;

		/* Did time go backwards? */
		if (unlikely(diff < 0))
		{
			fprintf(stderr, _("Detected clock going backwards in time.\n"));
			fprintf(stderr, _("Time warp: %d ms\n"), diff);
			exit(1);
		}

		/* What is the highest bit in the time diff? */
		if (diff > 0)
			bits = pg_leftmost_one_pos32(diff) + 1;
		else
			bits = 0;

		/* Update appropriate duration bucket */
		histogram[bits]++;

		/* Update direct histogram of time diffs */
		if (diff < NUM_DIRECT)
			direct_histogram[diff]++;

		/* Also track the largest observed duration, even if >= NUM_DIRECT */
		if (diff > largest_diff)
		{
			largest_diff = diff;
			largest_diff_count = 1;
		}
		else if (diff == largest_diff)
			largest_diff_count++;

		loop_count++;
		INSTR_TIME_SUBTRACT(temp, start_time);
		time_elapsed = INSTR_TIME_GET_NANOSEC(temp);
	}

	INSTR_TIME_SET_CURRENT(end_time);

	INSTR_TIME_SUBTRACT(end_time, start_time);

	printf(_("Average loop time including overhead: %0.2f ns\n"),
		   INSTR_TIME_GET_DOUBLE(end_time) * 1e9 / loop_count);

	return loop_count;
}

static void
output(uint64 loop_count)
{
	int			max_bit = 31;
	const char *header1 = _("<= ns");
	const char *header1b = _("ns");
	const char *header2 = /* xgettext:no-c-format */ _("% of total");
	const char *header3 = /* xgettext:no-c-format */ _("running %");
	const char *header4 = _("count");
	int			len1 = strlen(header1);
	int			len2 = strlen(header2);
	int			len3 = strlen(header3);
	int			len4 = strlen(header4);
	double		rprct;
	bool		stopped = false;

	/* find highest bit value */
	while (max_bit > 0 && histogram[max_bit] == 0)
		max_bit--;

	/* set minimum column widths */
	len1 = Max(8, len1);
	len2 = Max(10, len2);
	len3 = Max(10, len3);
	len4 = Max(10, len4);

	printf(_("Histogram of timing durations:\n"));
	printf("%*s   %*s %*s %*s\n",
		   len1, header1,
		   len2, header2,
		   len3, header3,
		   len4, header4);

	rprct = 0;
	for (int i = 0; i <= max_bit; i++)
	{
		double		prct = (double) histogram[i] * 100 / loop_count;

		rprct += prct;
		printf("%*ld   %*.4f %*.4f %*lld\n",
			   len1, (1L << i) - 1,
			   len2, prct,
			   len3, rprct,
			   len4, histogram[i]);
	}

	printf(_("\nObserved timing durations up to %.4f%%:\n"), max_rprct);
	printf("%*s   %*s %*s %*s\n",
		   len1, header1b,
		   len2, header2,
		   len3, header3,
		   len4, header4);

	rprct = 0;
	for (int i = 0; i < NUM_DIRECT; i++)
	{
		if (direct_histogram[i])
		{
			double		prct = (double) direct_histogram[i] * 100 / loop_count;
			bool		print_it = !stopped;

			rprct += prct;

			/* if largest diff is < NUM_DIRECT, be sure we print it */
			if (i == largest_diff)
			{
				if (stopped)
					printf("...\n");
				print_it = true;
			}

			if (print_it)
				printf("%*d   %*.4f %*.4f %*lld\n",
					   len1, i,
					   len2, prct,
					   len3, rprct,
					   len4, direct_histogram[i]);
			if (rprct >= max_rprct)
				stopped = true;
		}
	}

	/* print largest diff when it's outside the array range */
	if (largest_diff >= NUM_DIRECT)
	{
		double		prct = (double) largest_diff_count * 100 / loop_count;

		printf("...\n");
		printf("%*d   %*.4f %*.4f %*lld\n",
			   len1, largest_diff,
			   len2, prct,
			   len3, 100.0,
			   len4, largest_diff_count);
	}
}
