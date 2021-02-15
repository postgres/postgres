/*
 *	pg_test_fsync.c
 *		tests all supported fsync() methods
 */

#include "postgres_fe.h"

#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "access/xlogdefs.h"
#include "common/logging.h"
#include "getopt_long.h"

/*
 * put the temp files in the local directory
 * unless the user specifies otherwise
 */
#define FSYNC_FILENAME	"./pg_test_fsync.out"

#define XLOG_BLCKSZ_K	(XLOG_BLCKSZ / 1024)

#define LABEL_FORMAT		"        %-30s"
#define NA_FORMAT			"%21s\n"
/* translator: maintain alignment with NA_FORMAT */
#define OPS_FORMAT			gettext_noop("%13.3f ops/sec  %6.0f usecs/op\n")
#define USECS_SEC			1000000

/* These are macros to avoid timing the function call overhead. */
#ifndef WIN32
#define START_TIMER \
do { \
	alarm_triggered = false; \
	alarm(secs_per_test); \
	gettimeofday(&start_t, NULL); \
} while (0)
#else
/* WIN32 doesn't support alarm, so we create a thread and sleep there */
#define START_TIMER \
do { \
	alarm_triggered = false; \
	if (CreateThread(NULL, 0, process_alarm, NULL, 0, NULL) == \
		INVALID_HANDLE_VALUE) \
	{ \
		pg_log_error("could not create thread for alarm"); \
		exit(1); \
	} \
	gettimeofday(&start_t, NULL); \
} while (0)
#endif

#define STOP_TIMER	\
do { \
	gettimeofday(&stop_t, NULL); \
	print_elapse(start_t, stop_t, ops); \
} while (0)


static const char *progname;

static unsigned int secs_per_test = 5;
static int	needs_unlink = 0;
static char full_buf[DEFAULT_XLOG_SEG_SIZE],
		   *buf,
		   *filename = FSYNC_FILENAME;
static struct timeval start_t,
			stop_t;
static bool alarm_triggered = false;


static void handle_args(int argc, char *argv[]);
static void prepare_buf(void);
static void test_open(void);
static void test_non_sync(void);
static void test_sync(int writes_per_op);
static void test_open_syncs(void);
static void test_open_sync(const char *msg, int writes_size);
static void test_file_descriptor_sync(void);

#ifndef WIN32
static void process_alarm(int sig);
#else
static DWORD WINAPI process_alarm(LPVOID param);
#endif
static void signal_cleanup(int sig);

#ifdef HAVE_FSYNC_WRITETHROUGH
static int	pg_fsync_writethrough(int fd);
#endif
static void print_elapse(struct timeval start_t, struct timeval stop_t, int ops);

#define die(msg) do { pg_log_error("%s: %m", _(msg)); exit(1); } while(0)


int
main(int argc, char *argv[])
{
	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_test_fsync"));
	progname = get_progname(argv[0]);

	handle_args(argc, argv);

	/* Prevent leaving behind the test file */
	pqsignal(SIGINT, signal_cleanup);
	pqsignal(SIGTERM, signal_cleanup);
#ifndef WIN32
	pqsignal(SIGALRM, process_alarm);
#endif
#ifdef SIGHUP
	/* Not defined on win32 */
	pqsignal(SIGHUP, signal_cleanup);
#endif

	prepare_buf();

	test_open();

	/* Test using 1 XLOG_BLCKSZ write */
	test_sync(1);

	/* Test using 2 XLOG_BLCKSZ writes */
	test_sync(2);

	test_open_syncs();

	test_file_descriptor_sync();

	test_non_sync();

	unlink(filename);

	return 0;
}

static void
handle_args(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"filename", required_argument, NULL, 'f'},
		{"secs-per-test", required_argument, NULL, 's'},
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
			printf(_("Usage: %s [-f FILENAME] [-s SECS-PER-TEST]\n"), progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_test_fsync (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((option = getopt_long(argc, argv, "f:s:",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'f':
				filename = pg_strdup(optarg);
				break;

			case 's':
				errno = 0;
				optval = strtoul(optarg, &endptr, 10);

				if (endptr == optarg || *endptr != '\0' ||
					errno != 0 || optval != (unsigned int) optval)
				{
					pg_log_error("invalid argument for option %s", "--secs-per-test");
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}

				secs_per_test = (unsigned int) optval;
				if (secs_per_test == 0)
				{
					pg_log_error("%s must be in range %u..%u",
								 "--secs-per-test", 1, UINT_MAX);
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
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	printf(ngettext("%u second per test\n",
					"%u seconds per test\n",
					secs_per_test),
		   secs_per_test);
#if PG_O_DIRECT != 0
	printf(_("O_DIRECT supported on this platform for open_datasync and open_sync.\n"));
#else
	printf(_("Direct I/O is not supported on this platform.\n"));
#endif
}

static void
prepare_buf(void)
{
	int			ops;

	/* write random data into buffer */
	for (ops = 0; ops < DEFAULT_XLOG_SEG_SIZE; ops++)
		full_buf[ops] = random();

	buf = (char *) TYPEALIGN(XLOG_BLCKSZ, full_buf);
}

static void
test_open(void)
{
	int			tmpfile;

	/*
	 * test if we can open the target file
	 */
	if ((tmpfile = open(filename, O_RDWR | O_CREAT | PG_BINARY, S_IRUSR | S_IWUSR)) == -1)
		die("could not open output file");
	needs_unlink = 1;
	if (write(tmpfile, full_buf, DEFAULT_XLOG_SEG_SIZE) !=
		DEFAULT_XLOG_SEG_SIZE)
		die("write failed");

	/* fsync now so that dirty buffers don't skew later tests */
	if (fsync(tmpfile) != 0)
		die("fsync failed");

	close(tmpfile);
}

static void
test_sync(int writes_per_op)
{
	int			tmpfile,
				ops,
				writes;
	bool		fs_warning = false;

	if (writes_per_op == 1)
		printf(_("\nCompare file sync methods using one %dkB write:\n"), XLOG_BLCKSZ_K);
	else
		printf(_("\nCompare file sync methods using two %dkB writes:\n"), XLOG_BLCKSZ_K);
	printf(_("(in wal_sync_method preference order, except fdatasync is Linux's default)\n"));

	/*
	 * Test open_datasync if available
	 */
	printf(LABEL_FORMAT, "open_datasync");
	fflush(stdout);

#ifdef OPEN_DATASYNC_FLAG
	if ((tmpfile = open(filename, O_RDWR | O_DSYNC | PG_O_DIRECT | PG_BINARY, 0)) == -1)
	{
		printf(NA_FORMAT, _("n/a*"));
		fs_warning = true;
	}
	else
	{
		START_TIMER;
		for (ops = 0; alarm_triggered == false; ops++)
		{
			for (writes = 0; writes < writes_per_op; writes++)
				if (pg_pwrite(tmpfile,
							  buf,
							  XLOG_BLCKSZ,
							  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
					die("write failed");
		}
		STOP_TIMER;
		close(tmpfile);
	}
#else
	printf(NA_FORMAT, _("n/a"));
#endif

/*
 * Test fdatasync if available
 */
	printf(LABEL_FORMAT, "fdatasync");
	fflush(stdout);

#ifdef HAVE_FDATASYNC
	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (pg_pwrite(tmpfile,
						  buf,
						  XLOG_BLCKSZ,
						  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
				die("write failed");
		fdatasync(tmpfile);
	}
	STOP_TIMER;
	close(tmpfile);
#else
	printf(NA_FORMAT, _("n/a"));
#endif

/*
 * Test fsync
 */
	printf(LABEL_FORMAT, "fsync");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (pg_pwrite(tmpfile,
						  buf,
						  XLOG_BLCKSZ,
						  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
				die("write failed");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
	}
	STOP_TIMER;
	close(tmpfile);

/*
 * If fsync_writethrough is available, test as well
 */
	printf(LABEL_FORMAT, "fsync_writethrough");
	fflush(stdout);

#ifdef HAVE_FSYNC_WRITETHROUGH
	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (pg_pwrite(tmpfile,
						  buf,
						  XLOG_BLCKSZ,
						  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
				die("write failed");
		if (pg_fsync_writethrough(tmpfile) != 0)
			die("fsync failed");
	}
	STOP_TIMER;
	close(tmpfile);
#else
	printf(NA_FORMAT, _("n/a"));
#endif

/*
 * Test open_sync if available
 */
	printf(LABEL_FORMAT, "open_sync");
	fflush(stdout);

#ifdef OPEN_SYNC_FLAG
	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG | PG_O_DIRECT | PG_BINARY, 0)) == -1)
	{
		printf(NA_FORMAT, _("n/a*"));
		fs_warning = true;
	}
	else
	{
		START_TIMER;
		for (ops = 0; alarm_triggered == false; ops++)
		{
			for (writes = 0; writes < writes_per_op; writes++)
				if (pg_pwrite(tmpfile,
							  buf,
							  XLOG_BLCKSZ,
							  writes * XLOG_BLCKSZ) != XLOG_BLCKSZ)
					/*
					 * This can generate write failures if the filesystem has
					 * a large block size, e.g. 4k, and there is no support
					 * for O_DIRECT writes smaller than the file system block
					 * size, e.g. XFS.
					 */
					die("write failed");
		}
		STOP_TIMER;
		close(tmpfile);
	}
#else
	printf(NA_FORMAT, _("n/a"));
#endif

	if (fs_warning)
	{
		printf(_("* This file system and its mount options do not support direct\n"
				 "  I/O, e.g. ext4 in journaled mode.\n"));
	}
}

static void
test_open_syncs(void)
{
	printf(_("\nCompare open_sync with different write sizes:\n"));
	printf(_("(This is designed to compare the cost of writing 16kB in different write\n"
			 "open_sync sizes.)\n"));

	test_open_sync(_(" 1 * 16kB open_sync write"), 16);
	test_open_sync(_(" 2 *  8kB open_sync writes"), 8);
	test_open_sync(_(" 4 *  4kB open_sync writes"), 4);
	test_open_sync(_(" 8 *  2kB open_sync writes"), 2);
	test_open_sync(_("16 *  1kB open_sync writes"), 1);
}

/*
 * Test open_sync with different size files
 */
static void
test_open_sync(const char *msg, int writes_size)
{
#ifdef OPEN_SYNC_FLAG
	int			tmpfile,
				ops,
				writes;
#endif

	printf(LABEL_FORMAT, msg);
	fflush(stdout);

#ifdef OPEN_SYNC_FLAG
	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG | PG_O_DIRECT | PG_BINARY, 0)) == -1)
		printf(NA_FORMAT, _("n/a*"));
	else
	{
		START_TIMER;
		for (ops = 0; alarm_triggered == false; ops++)
		{
			for (writes = 0; writes < 16 / writes_size; writes++)
				if (pg_pwrite(tmpfile,
							  buf,
							  writes_size * 1024,
							  writes * writes_size * 1024) !=
					writes_size * 1024)
					die("write failed");
		}
		STOP_TIMER;
		close(tmpfile);
	}
#else
	printf(NA_FORMAT, _("n/a"));
#endif
}

static void
test_file_descriptor_sync(void)
{
	int			tmpfile,
				ops;

	/*
	 * Test whether fsync can sync data written on a different descriptor for
	 * the same file.  This checks the efficiency of multi-process fsyncs
	 * against the same file. Possibly this should be done with writethrough
	 * on platforms which support it.
	 */
	printf(_("\nTest if fsync on non-write file descriptor is honored:\n"));
	printf(_("(If the times are similar, fsync() can sync data written on a different\n"
			 "descriptor.)\n"));

	/*
	 * first write, fsync and close, which is the normal behavior without
	 * multiple descriptors
	 */
	printf(LABEL_FORMAT, "write, fsync, close");
	fflush(stdout);

	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		if (write(tmpfile, buf, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			die("write failed");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		close(tmpfile);

		/*
		 * open and close the file again to be consistent with the following
		 * test
		 */
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		close(tmpfile);
	}
	STOP_TIMER;

	/*
	 * Now open, write, close, open again and fsync This simulates processes
	 * fsyncing each other's writes.
	 */
	printf(LABEL_FORMAT, "write, close, fsync");
	fflush(stdout);

	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		if (write(tmpfile, buf, XLOG_BLCKSZ) != XLOG_BLCKSZ)
			die("write failed");
		close(tmpfile);
		/* reopen file */
		if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
			die("could not open output file");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		close(tmpfile);
	}
	STOP_TIMER;
}

static void
test_non_sync(void)
{
	int			tmpfile,
				ops;

	/*
	 * Test a simple write without fsync
	 */
	printf(_("\nNon-sync'ed %dkB writes:\n"), XLOG_BLCKSZ_K);
	printf(LABEL_FORMAT, "write");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | PG_BINARY, 0)) == -1)
		die("could not open output file");
	START_TIMER;
	for (ops = 0; alarm_triggered == false; ops++)
	{
		if (pg_pwrite(tmpfile, buf, XLOG_BLCKSZ, 0) != XLOG_BLCKSZ)
			die("write failed");
	}
	STOP_TIMER;
	close(tmpfile);
}

static void
signal_cleanup(int signum)
{
	/* Delete the file if it exists. Ignore errors */
	if (needs_unlink)
		unlink(filename);
	/* Finish incomplete line on stdout */
	puts("");
	exit(signum);
}

#ifdef HAVE_FSYNC_WRITETHROUGH

static int
pg_fsync_writethrough(int fd)
{
#ifdef WIN32
	return _commit(fd);
#elif defined(F_FULLFSYNC)
	return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
	errno = ENOSYS;
	return -1;
#endif
}
#endif

/*
 * print out the writes per second for tests
 */
static void
print_elapse(struct timeval start_t, struct timeval stop_t, int ops)
{
	double		total_time = (stop_t.tv_sec - start_t.tv_sec) +
	(stop_t.tv_usec - start_t.tv_usec) * 0.000001;
	double		per_second = ops / total_time;
	double		avg_op_time_us = (total_time / ops) * USECS_SEC;

	printf(_(OPS_FORMAT), per_second, avg_op_time_us);
}

#ifndef WIN32
static void
process_alarm(int sig)
{
	alarm_triggered = true;
}
#else
static DWORD WINAPI
process_alarm(LPVOID param)
{
	/* WIN32 doesn't support alarm, so we create a thread and sleep here */
	Sleep(secs_per_test * 1000);
	alarm_triggered = true;
	ExitThread(0);
}
#endif
