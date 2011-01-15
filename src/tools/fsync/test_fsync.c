/*
 * src/tools/fsync/test_fsync.c
 *
 *
 *	test_fsync.c
 *		tests all supported fsync() methods
 */

#include "postgres.h"

#include "access/xlog_internal.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>


/* 
 * put the temp files in the local directory
 * unless the user specifies otherwise 
 */
#define FSYNC_FILENAME	"./test_fsync.out"

#define WRITE_SIZE	(8 * 1024)	/* 8k */

#define LABEL_FORMAT	"        %-32s"
#define NA_FORMAT		LABEL_FORMAT "%18s"


int			ops_per_test = 2000;
char	    full_buf[XLOG_SEG_SIZE], *buf, *filename = FSYNC_FILENAME;
struct timeval start_t, stop_t;


void		handle_args(int argc, char *argv[]);
void		prepare_buf(void);
void		test_open(void);
void		test_non_sync(void);
void		test_sync(int writes_per_op);
void		test_open_syncs(void);
void		test_file_descriptor_sync(void);
void		print_elapse(struct timeval start_t, struct timeval stop_t);
void		die(char *str);


int
main(int argc, char *argv[])
{
	handle_args(argc, argv);
	
	prepare_buf();

	test_open();
	
	test_non_sync();
	
	/* Test using 1 8k write */
	test_sync(1);

	/* Test using 2 8k writes */
	test_sync(2);
	
	test_open_syncs();

	test_file_descriptor_sync();
	
	unlink(filename);

	return 0;
}

void
handle_args(int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "-h") == 0)
	{
		fprintf(stderr, "test_fsync [-f filename] [ops-per-test]\n");
		exit(1);
	}
	
	/* 
	 * arguments: ops_per_test and filename (optional) 
	 */
	if (argc > 2 && strcmp(argv[1], "-f") == 0)
	{
		filename = argv[2];
		argv += 2;
		argc -= 2;
	}

	if (argc > 1) 
		ops_per_test = atoi(argv[1]);

	printf("Ops-per-test = %d\n\n", ops_per_test);
}

void
prepare_buf(void)
{
	int			ops;

	/* write random data into buffer */
	for (ops = 0; ops < XLOG_SEG_SIZE; ops++)
		full_buf[ops] = random();

	buf = (char *) TYPEALIGN(ALIGNOF_XLOG_BUFFER, full_buf);
}

void
test_open(void)
{
	int			tmpfile;

	/* 
	 * test if we can open the target file 
	 */
	if ((tmpfile = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) == -1)
		die("Cannot open output file.");
	if (write(tmpfile, full_buf, XLOG_SEG_SIZE) != XLOG_SEG_SIZE)
		die("write failed");

	/* fsync now so that dirty buffers don't skew later tests */
	if (fsync(tmpfile) != 0)
		die("fsync failed");

	close(tmpfile);
}

void
test_non_sync(void)
{
	int			tmpfile, ops;

	/*
	 * Test a simple write without fsync
	 */
	printf("Simple non-sync'ed write:\n");
	printf(LABEL_FORMAT, "8k write");
	fflush(stdout);

	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		close(tmpfile);
	}
	gettimeofday(&stop_t, NULL);
	print_elapse(start_t, stop_t);
}

void
test_sync(int writes_per_op)
{
	int			tmpfile, ops, writes;

	if (writes_per_op == 1)
		printf("\nCompare file sync methods using one write:\n");
	else
		printf("\nCompare file sync methods using two writes:\n");
	printf("(in wal_sync_method preference order, except fdatasync\n");
	printf("is Linux's default)\n");

	/*
	 * Test open_datasync if available
	 */
#ifdef OPEN_DATASYNC_FLAG
	if (writes_per_op == 1)
		printf(LABEL_FORMAT, "open_datasync 8k write");
	else
	 	printf(LABEL_FORMAT, "2 open_datasync 8k writes");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | O_DSYNC, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
				die("write failed");
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);

	/*
	 * If O_DIRECT is enabled, test that with open_datasync
	 */
#if PG_O_DIRECT != 0
	if ((tmpfile = open(filename, O_RDWR | O_DSYNC | PG_O_DIRECT, 0)) == -1)
		printf(NA_FORMAT, "o_direct", "n/a on this filesystem\n");
	else
	{
		if (writes_per_op == 1)
			printf(LABEL_FORMAT, "open_datasync 8k direct I/O write");
		else
			printf(LABEL_FORMAT, "2 open_datasync 8k direct I/O writes");
		fflush(stdout);

		gettimeofday(&start_t, NULL);
		for (ops = 0; ops < ops_per_test; ops++)
		{
			for (writes = 0; writes < writes_per_op; writes++)
				if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
					die("write failed");
			if (lseek(tmpfile, 0, SEEK_SET) == -1)
				die("seek failed");
		}
		gettimeofday(&stop_t, NULL);
		close(tmpfile);
		print_elapse(start_t, stop_t);
	}
#else
		printf(NA_FORMAT, "o_direct", "n/a\n");
#endif

#else
	printf(NA_FORMAT, "open_datasync", "n/a\n");
#endif

/*
 * Test fdatasync if available
 */
#ifdef HAVE_FDATASYNC
	if (writes_per_op == 1)
		printf(LABEL_FORMAT, "8k write, fdatasync");
	else
		printf(LABEL_FORMAT, "8k write, 8k write, fdatasync");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
				die("write failed");
		fdatasync(tmpfile);
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);
#else
	printf(NA_FORMAT, "fdatasync", "n/a\n");
#endif

/*
 * Test fsync
 */
	if (writes_per_op == 1)
		printf(LABEL_FORMAT, "8k write, fsync");
	else
		printf(LABEL_FORMAT, "8k write, 8k write, fsync");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
				die("write failed");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);
	
/*
 * If fsync_writethrough is available, test as well
 */	
#ifdef HAVE_FSYNC_WRITETHROUGH
	if (writes_per_op == 1)
		printf(LABEL_FORMAT, "8k write, fsync_writethrough");
	else
		printf(LABEL_FORMAT, "8k write, 8k write, fsync_writethrough");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
				die("write failed");
		if (fcntl(tmpfile, F_FULLFSYNC ) != 0)
			die("fsync failed");
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);
#else
	printf(NA_FORMAT, "fsync_writethrough", "n/a\n");
#endif

/*
 * Test open_sync if available
 */
#ifdef OPEN_SYNC_FLAG
	if (writes_per_op == 1)
		printf(LABEL_FORMAT, "open_sync 8k write");
	else
		printf(LABEL_FORMAT, "2 open_sync 8k writes");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		for (writes = 0; writes < writes_per_op; writes++)
			if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
				die("write failed");
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);

	/*
	 * If O_DIRECT is enabled, test that with open_sync
	 */
#if PG_O_DIRECT != 0
	if (writes_per_op == 1)
		printf(LABEL_FORMAT, "open_sync 8k direct I/O write");
	else
		printf(LABEL_FORMAT, "2 open_sync 8k direct I/O writes");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG | PG_O_DIRECT, 0)) == -1)
		printf(NA_FORMAT, "o_direct", "n/a on this filesystem\n");
	else
	{
		gettimeofday(&start_t, NULL);
		for (ops = 0; ops < ops_per_test; ops++)
		{
			for (writes = 0; writes < writes_per_op; writes++)
				if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
					die("write failed");
			if (lseek(tmpfile, 0, SEEK_SET) == -1)
				die("seek failed");
		}
		gettimeofday(&stop_t, NULL);
		close(tmpfile);
		print_elapse(start_t, stop_t);
	}
#else
	printf(NA_FORMAT, "o_direct", "n/a\n");
#endif

#else
	printf(NA_FORMAT, "open_sync", "n/a\n");
#endif
}

void
test_open_syncs(void)
{
	int			tmpfile, ops;

	/*
	 * Compare 1 to 2 writes
	 */
	printf("\nCompare open_sync with different sizes:\n");
	printf("(This is designed to compare the cost of one large\n");
	printf("sync'ed write and two smaller sync'ed writes.)\n");

/*
 * Test open_sync with different size files
 */
#ifdef OPEN_SYNC_FLAG
	printf(LABEL_FORMAT, "open_sync 16k write");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		if (write(tmpfile, buf, WRITE_SIZE * 2) != WRITE_SIZE * 2)
			die("write failed");
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);

	printf(LABEL_FORMAT, "2 open_sync 8k writes");
	fflush(stdout);

	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		if (lseek(tmpfile, 0, SEEK_SET) == -1)
			die("seek failed");
	}
	gettimeofday(&stop_t, NULL);
	close(tmpfile);
	print_elapse(start_t, stop_t);
#else
	printf(NA_FORMAT, "open_sync", "n/a\n");
#endif
}

void
test_file_descriptor_sync(void)
{
	int			tmpfile, ops;

	/*
	 * Test whether fsync can sync data written on a different
	 * descriptor for the same file.  This checks the efficiency
	 * of multi-process fsyncs against the same file.
	 * Possibly this should be done with writethrough on platforms
	 * which support it.
	 */
	printf("\nTest if fsync on non-write file descriptor is honored:\n");
	printf("(If the times are similar, fsync() can sync data written\n");
	printf("on a different descriptor.)\n");

	/* 
	 * first write, fsync and close, which is the 
	 * normal behavior without multiple descriptors
	 */
	printf(LABEL_FORMAT, "8k write, fsync, close");
	fflush(stdout);

	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		close(tmpfile);
		/*
		 * open and close the file again to be consistent
		 * with the following test
		 */
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		close(tmpfile);
	}
	gettimeofday(&stop_t, NULL);
	print_elapse(start_t, stop_t);

	/*
	 * Now open, write, close, open again and fsync
	 * This simulates processes fsyncing each other's
	 * writes.
	 */
 	printf(LABEL_FORMAT, "8k write, close, fsync");
 	fflush(stdout);

	gettimeofday(&start_t, NULL);
	for (ops = 0; ops < ops_per_test; ops++)
	{
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		close(tmpfile);
		/* reopen file */
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		if (fsync(tmpfile) != 0)
			die("fsync failed");
		close(tmpfile);
	}
	gettimeofday(&stop_t, NULL);
	print_elapse(start_t, stop_t);

}

/* 
 * print out the writes per second for tests
 */
void
print_elapse(struct timeval start_t, struct timeval stop_t)
{
	double		total_time = (stop_t.tv_sec - start_t.tv_sec) +
	(stop_t.tv_usec - start_t.tv_usec) * 0.000001;
	double		per_second = ops_per_test / total_time;

	printf("%9.3f ops/sec\n", per_second);
}

void
die(char *str)
{
	fprintf(stderr, "%s\n", str);
	exit(1);
}
