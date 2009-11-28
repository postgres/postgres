/*
 * $PostgreSQL: pgsql/src/tools/fsync/test_fsync.c,v 1.26 2009/11/28 15:04:54 momjian Exp $
 *
 *
 *	test_fsync.c
 *		test various fsync() methods
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


#ifdef WIN32
#define FSYNC_FILENAME	"./test_fsync.out"
#else
/* /tmp might be a memory file system */
#define FSYNC_FILENAME	"/var/tmp/test_fsync.out"
#endif

#define WRITE_SIZE	(8 * 1024) /* 8k */

#define LABEL_FORMAT	"\t%-30s"

void		die(char *str);
void		print_elapse(struct timeval start_t, struct timeval stop_t);

int
main(int argc, char *argv[])
{
	struct timeval start_t;
	struct timeval stop_t;
	int			tmpfile,
				i,
				loops = 5000;
	char	   *full_buf = (char *) malloc(XLOG_SEG_SIZE),
			   *buf;
	char	   *filename = FSYNC_FILENAME;

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

	if ((tmpfile = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR, 0)) == -1)
		die("Cannot open output file.");
	if (write(tmpfile, full_buf, XLOG_SEG_SIZE) != XLOG_SEG_SIZE)
		die("write failed");
	/* fsync now so later fsync's don't have to do it */
	if (fsync(tmpfile) != 0)
		die("fsync failed");
	close(tmpfile);

	buf = (char *) TYPEALIGN(ALIGNOF_XLOG_BUFFER, full_buf);

	/*
	 *	Simple write
	 */
	printf("Simple 8k write timing:\n");
	/* write only */
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
	printf(LABEL_FORMAT, "write");
	print_elapse(start_t, stop_t);

	/*
	 *	Compare file sync methods with one 8k write
	 */
	printf("\nCompare file sync methods using one 8k write:\n");

#ifdef OPEN_DATASYNC_FLAG
	/* open_dsync, write */
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
	printf(LABEL_FORMAT, "open_datasync write");
	print_elapse(start_t, stop_t);
#else
	printf("\t(open_datasync unavailable)\n");
#endif

#ifdef OPEN_SYNC_FLAG
	/* open_fsync, write */
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
	printf(LABEL_FORMAT, "open_sync write");
	print_elapse(start_t, stop_t);
#else
	printf("\t(open_sync unavailable)\n");
#endif

#ifdef HAVE_FDATASYNC
	/* write, fdatasync */
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
	printf(LABEL_FORMAT, "write, fdatasync");
	print_elapse(start_t, stop_t);
#else
	printf("\t(fdatasync unavailable)\n");
#endif

	/* write, fsync, close */
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
	printf(LABEL_FORMAT, "write, fsync");
	print_elapse(start_t, stop_t);

	/*
	 *	Compare file sync methods with two 8k write
	 */
	printf("\nCompare file sync methods using two 8k writes:\n");

#ifdef OPEN_DATASYNC_FLAG
	/* open_dsync, write */
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
	printf(LABEL_FORMAT, "open_datasync write, write");
	print_elapse(start_t, stop_t);
#else
	printf("\t(open_datasync unavailable)\n");
#endif

#ifdef OPEN_SYNC_FLAG
	/* open_fsync, write */
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
	printf(LABEL_FORMAT, "open_sync write, write");
	print_elapse(start_t, stop_t);
#endif

#ifdef HAVE_FDATASYNC
	/* write, fdatasync */
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
	printf(LABEL_FORMAT, "write, write, fdatasync");
	print_elapse(start_t, stop_t);
#else
	printf("\t(fdatasync unavailable)\n");
#endif

	/* write, fsync, close */
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
	printf(LABEL_FORMAT, "write, write, fsync");
	print_elapse(start_t, stop_t);

	/*
	 *	Compare 1 to 2 writes
	 */
	printf("\nCompare open_sync sizes:\n");

#ifdef OPEN_SYNC_FLAG
	/* 16k open_sync write */
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
	printf(LABEL_FORMAT, "16k open_sync write");
	print_elapse(start_t, stop_t);

	/* Two 8k open_sync writes */
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
	printf(LABEL_FORMAT, "2 8k open_sync writes");
	print_elapse(start_t, stop_t);
#else
	printf("\t(open_sync unavailable)\n");
#endif

	/*
	 *	Fsync another file descriptor?
	 */
	printf("\nCompare fsync times on write() and new file descriptors (if the times\n");
	printf("are similar, fsync() can sync data written on a different descriptor):\n");

	/* write, fsync, close */
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
		if ((tmpfile = open(filename, O_RDWR, 0)) == -1)
			die("Cannot open output file.");
		/* do nothing but the open/close the tests are consistent. */
		close(tmpfile);
	}
	gettimeofday(&stop_t, NULL);
	printf(LABEL_FORMAT, "write, fsync, close");
	print_elapse(start_t, stop_t);

	/* write, close, fsync */
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
	printf(LABEL_FORMAT, "write, close, fsync");
	print_elapse(start_t, stop_t);

	/* cleanup */
	free(full_buf);
	unlink(filename);

	return 0;
}

void
print_elapse(struct timeval start_t, struct timeval stop_t)
{
	if (stop_t.tv_usec < start_t.tv_usec)
	{
		stop_t.tv_sec--;
		stop_t.tv_usec += 1000000;
	}

	printf("%3ld.%06ld\n", (long) (stop_t.tv_sec - start_t.tv_sec),
		   (long) (stop_t.tv_usec - start_t.tv_usec));
}

void
die(char *str)
{
	fprintf(stderr, "%s\n", str);
	exit(1);
}
