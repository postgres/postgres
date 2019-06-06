/*
 *	pg_test_timing.c
 *		tests overhead of timing calls and their monotonicity:	that
 *		they always move forward
 */

#include "postgres_fe.h"

#include "getopt_long.h"
#include "portability/instr_time.h"

static const char *progname;

static int32 test_duration = 3;

static void handle_args(int argc, char *argv[]);
static uint64 test_timing(int32);
static void output(uint64 loop_count);

/* record duration in powers of 2 microseconds */
long long int histogram[32];

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
		{NULL, 0, NULL, 0}
	};

	int			option;			/* Command line option */
	int			optindex = 0;	/* used by getopt_long */

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			printf(_("Usage: %s [-d DURATION]\n"), progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_test_timing (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((option = getopt_long(argc, argv, "d:",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'd':
				test_duration = atoi(optarg);
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

	if (test_duration > 0)
	{
		printf(ngettext("Testing timing overhead for %d second.\n",
						"Testing timing overhead for %d seconds.\n",
						test_duration),
			   test_duration);
	}
	else
	{
		fprintf(stderr,
				_("%s: duration must be a positive integer (duration is \"%d\")\n"),
				progname, test_duration);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}
}

static uint64
test_timing(int32 duration)
{
	uint64		total_time;
	int64		time_elapsed = 0;
	uint64		loop_count = 0;
	uint64		prev,
				cur;
	instr_time	start_time,
				end_time,
				temp;

	total_time = duration > 0 ? duration * INT64CONST(1000000) : 0;

	INSTR_TIME_SET_CURRENT(start_time);
	cur = INSTR_TIME_GET_MICROSEC(start_time);

	while (time_elapsed < total_time)
	{
		int32		diff,
					bits = 0;

		prev = cur;
		INSTR_TIME_SET_CURRENT(temp);
		cur = INSTR_TIME_GET_MICROSEC(temp);
		diff = cur - prev;

		/* Did time go backwards? */
		if (diff < 0)
		{
			fprintf(stderr, _("Detected clock going backwards in time.\n"));
			fprintf(stderr, _("Time warp: %d ms\n"), diff);
			exit(1);
		}

		/* What is the highest bit in the time diff? */
		while (diff)
		{
			diff >>= 1;
			bits++;
		}

		/* Update appropriate duration bucket */
		histogram[bits]++;

		loop_count++;
		INSTR_TIME_SUBTRACT(temp, start_time);
		time_elapsed = INSTR_TIME_GET_MICROSEC(temp);
	}

	INSTR_TIME_SET_CURRENT(end_time);

	INSTR_TIME_SUBTRACT(end_time, start_time);

	printf(_("Per loop time including overhead: %0.2f ns\n"),
		   INSTR_TIME_GET_DOUBLE(end_time) * 1e9 / loop_count);

	return loop_count;
}

static void
output(uint64 loop_count)
{
	int64		max_bit = 31,
				i;
	char	   *header1 = _("< us");
	char	   *header2 = /* xgettext:no-c-format */ _("% of total");
	char	   *header3 = _("count");
	int			len1 = strlen(header1);
	int			len2 = strlen(header2);
	int			len3 = strlen(header3);

	/* find highest bit value */
	while (max_bit > 0 && histogram[max_bit] == 0)
		max_bit--;

	printf(_("Histogram of timing durations:\n"));
	printf("%*s   %*s %*s\n",
		   Max(6, len1), header1,
		   Max(10, len2), header2,
		   Max(10, len3), header3);

	for (i = 0; i <= max_bit; i++)
		printf("%*ld    %*.5f %*lld\n",
			   Max(6, len1), 1l << i,
			   Max(10, len2) - 1, (double) histogram[i] * 100 / loop_count,
			   Max(10, len3), histogram[i]);
}
