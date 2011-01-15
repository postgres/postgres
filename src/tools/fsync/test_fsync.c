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


int			loops = 2000;

void		die(char *str);
void		print_elapse(struct timeval start_t, struct timeval stop_t);

int
main(int argc, char *argv[])
{
	struct timeval start_t, stop_t;
	int			tmpfile, i;
	char	   *full_buf = (char *) malloc(XLOG_SEG_SIZE),
			   *buf, *filename = FSYNC_FILENAME;

	/* 
	 * arguments: loops and filename (optional) 
	 */
	if (argc > 2 && strcmp(argv[1], "-f") == 0)
	{
		filename = argv[2];
		argv += 2;
		argc -= 2;
	}

	if (argc > 1) 
		loops = atoi(argv[1]);

	for (i = 0; i < XLOG_SEG_SIZE; i++)
		full_buf[i] = random();

	/* 
	 * test if we can open the target file 
	 */
	if ((tmpfile = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) == -1)
		die("Cannot open output file.");
	if (write(tmpfile, full_buf, XLOG_SEG_SIZE) != XLOG_SEG_SIZE)
		die("write failed");
	/*
	 * fsync now so that dirty buffers don't skew later tests
	 */
	if (fsync(tmpfile) != 0)
		die("fsync failed");
	close(tmpfile);

	buf = (char *) TYPEALIGN(ALIGNOF_XLOG_BUFFER, full_buf);

	printf("Loops = %d\n\n", loops);

	/*
	 * Test a simple write without fsync
	 */
	printf("Simple write:\n");
	printf(LABEL_FORMAT, "8k write");
	fflush(stdout);
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		close(tmpfile);
	}
	gettimeofday(&stop_t, NULL);
	print_elapse(start_t, stop_t);

	/*
	 * Test all fsync methods using single 8k writes
	 */
	printf("\nCompare file sync methods using one write:\n");

	/*
	 * Test open_datasync if available
	 */
#ifdef OPEN_DATASYNC_FLAG
	printf(LABEL_FORMAT, "open_datasync 8k write");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | O_DSYNC, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
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
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | O_DSYNC | PG_O_DIRECT, 0)) == -1)
		printf(NA_FORMAT, "o_direct", "n/a on this filesystem\n");
	else
	{
		printf(LABEL_FORMAT, "open_datasync 8k direct I/O write");
		gettimeofday(&start_t, NULL);
		for (i = 0; i < loops; i++)
		{
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
 * Test open_sync if available
 */
#ifdef OPEN_SYNC_FLAG
	printf(LABEL_FORMAT, "open_sync 8k write");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
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
	printf(LABEL_FORMAT, "open_sync 8k direct I/O write");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG | PG_O_DIRECT, 0)) == -1)
		printf(NA_FORMAT, "o_direct", "n/a on this filesystem\n");
	else
	{
		gettimeofday(&start_t, NULL);
		for (i = 0; i < loops; i++)
		{
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

/*
 * Test fdatasync if available
 */
#ifdef HAVE_FDATASYNC
	printf(LABEL_FORMAT, "8k write, fdatasync");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
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
	printf(LABEL_FORMAT, "8k write, fsync");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
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
	printf(LABEL_FORMAT, "8k write, fsync_writethrough");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
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
	 * Compare some of the file sync methods with 
	 * two 8k writes to see if timing is different
	 */
	printf("\nCompare file sync methods using two writes:\n");

/*
 * Test open_datasync with and without o_direct
 */
#ifdef OPEN_DATASYNC_FLAG
 	printf(LABEL_FORMAT, "2 open_datasync 8k writes");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | O_DSYNC, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
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
	
#if PG_O_DIRECT != 0
	printf(LABEL_FORMAT, "2 open_datasync direct I/O 8k writes");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | O_DSYNC | PG_O_DIRECT, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
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
		printf(NA_FORMAT, "o_direct" "n/a\n");
#endif

#else
	printf(NA_FORMAT, "open_datasync", "n/a\n");
#endif

/*
 * Test open_sync with and without o_direct
 */
#ifdef OPEN_SYNC_FLAG
	printf(LABEL_FORMAT, "2 open_sync 8k writes");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
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
	
#if PG_O_DIRECT != 0
	printf(LABEL_FORMAT, "2 open_sync direct I/O 8k writes");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR | OPEN_SYNC_FLAG | PG_O_DIRECT, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
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
	printf(NA_FORMAT, "o_direct", "n/a\n");
#endif

#else
	printf(NA_FORMAT, "open_sync", "n/a\n");
#endif

/*
 *	Test fdatasync
 */
#ifdef HAVE_FDATASYNC
	printf(LABEL_FORMAT, "8k write, 8k write, fdatasync");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
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
 * Test basic fsync
 */
	printf(LABEL_FORMAT, "8k write, 8k write, fsync");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
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
 * Test fsync_writethrough if available
 */	
#ifdef HAVE_FSYNC_WRITETHROUGH
	printf(LABEL_FORMAT, "8k write, 8k write, fsync_writethrough");
	fflush(stdout);
	if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
		die("Cannot open output file.");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		if (write(tmpfile, buf, WRITE_SIZE) != WRITE_SIZE)
			die("write failed");
		if (fcntl(tmpfile, F_FULLFSYNC) != 0)
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
	for (i = 0; i < loops; i++)
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
	for (i = 0; i < loops; i++)
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
	for (i = 0; i < loops; i++)
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
	for (i = 0; i < loops; i++)
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

	/* 
	 * cleanup 
	 */
	free(full_buf);
	unlink(filename);

	return 0;
}

/* 
 * print out the writes per second for tests
 */
void
print_elapse(struct timeval start_t, struct timeval stop_t)
{
	double		total_time = (stop_t.tv_sec - start_t.tv_sec) +
	(stop_t.tv_usec - start_t.tv_usec) * 0.000001;
	double		per_second = loops / total_time;

	printf("%9.3f ops/sec\n", per_second);
}

void
die(char *str)
{
	fprintf(stderr, "%s\n", str);
	exit(1);
}
