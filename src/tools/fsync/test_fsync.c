/*
 *	test_fsync.c
 *		tests if fsync can be done from another process than the original write
 */

#include "../../include/pg_config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#define FSYNC_FILENAME	"/var/tmp/test_fsync.out"

/* O_SYNC and O_FSYNC are the same */
#if defined(O_SYNC)
#define OPEN_SYNC_FLAG		O_SYNC
#elif defined(O_FSYNC)
#define OPEN_SYNC_FLAG		O_FSYNC
#endif

#if defined(OPEN_SYNC_FLAG)
#if defined(O_DSYNC) && (O_DSYNC != OPEN_SYNC_FLAG)
#define OPEN_DATASYNC_FLAG	O_DSYNC
#endif
#endif

void		die(char *str);
void		print_elapse(struct timeval start_t, struct timeval elapse_t);

int
main(int argc, char *argv[])
{
	struct timeval start_t;
	struct timeval elapse_t;
	int			tmpfile,
				i,
				loops=1000;
	char	   *strout = (char *) malloc(65536);
	char	   *filename = FSYNC_FILENAME;

	if (argc > 2 && strcmp(argv[1],"-f") == 0)
	{
		filename = argv[2];
		argv += 2;
		argc -= 2;
	}
		
	if (argc > 1)
			loops = atoi(argv[1]);
		
	for (i = 0; i < 65536; i++)
		strout[i] = 'a';

	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, strout, 65536);
	fsync(tmpfile);				/* fsync so later fsync's don't have to do
								 * it */
	close(tmpfile);

	printf("Simple write timing:\n");
	/* write only */
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
			die("can't open /var/tmp/test_fsync.out");
		write(tmpfile, strout, 8192);
		close(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	printf("\twrite                  ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	printf("\nCompare fsync times on write() and non-write() descriptor:\n");
	printf("(If the times are similar, fsync() can sync data written\n on a different descriptor.)\n");

	/* write, fsync, close */
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
			die("can't open /var/tmp/test_fsync.out");
		write(tmpfile, strout, 8192);
		fsync(tmpfile);
		close(tmpfile);
		if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
			die("can't open /var/tmp/test_fsync.out");
		/* do nothing but the open/close the tests are consistent. */
		close(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	printf("\twrite, fsync, close    ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	/* write, close, fsync */
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
			die("can't open /var/tmp/test_fsync.out");
		write(tmpfile, strout, 8192);
		close(tmpfile);
		/* reopen file */
		if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
			die("can't open /var/tmp/test_fsync.out");
		fsync(tmpfile);
		close(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	printf("\twrite, close, fsync    ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	printf("\nCompare one o_sync write to two:\n");

	/* 16k o_sync write */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | OPEN_SYNC_FLAG)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
		write(tmpfile, strout, 16384);
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\tone 16k o_sync write   ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	/* 2*8k o_sync writes */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | OPEN_SYNC_FLAG)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		write(tmpfile, strout, 8192);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\ttwo 8k o_sync writes   ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	printf("\nCompare file sync methods with one 8k write:\n");

#ifdef OPEN_DATASYNC_FLAG
	/* open_dsync, write */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | O_DSYNC)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
		write(tmpfile, strout, 8192);
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\topen o_dsync, write    ");
	print_elapse(start_t, elapse_t);
#else
	printf("\t(o_dsync unavailable)  ");
#endif
	printf("\n");

	/* open_fsync, write */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | OPEN_SYNC_FLAG)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
		write(tmpfile, strout, 8192);
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\topen o_sync, write     ");
	print_elapse(start_t, elapse_t);
	printf("\n");

#ifdef HAVE_FDATASYNC
	/* write, fdatasync */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		fdatasync(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\twrite, fdatasync       ");
	print_elapse(start_t, elapse_t);
#else
	printf("\t(fdatasync unavailable)");
#endif
	printf("\n");

	/* write, fsync, close */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		fsync(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\twrite, fsync,          ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	printf("\nCompare file sync methods with 2 8k writes:\n");
	printf("(The fastest should be used for wal_sync_method)\n");

#ifdef OPEN_DATASYNC_FLAG
	/* open_dsync, write */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | O_DSYNC)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		write(tmpfile, strout, 8192);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\topen o_dsync, write    ");
	print_elapse(start_t, elapse_t);
#else
	printf("\t(o_dsync unavailable)  ");
#endif
	printf("\n");

	/* open_fsync, write */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR | OPEN_SYNC_FLAG)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		write(tmpfile, strout, 8192);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\topen o_sync, write     ");
	print_elapse(start_t, elapse_t);
	printf("\n");

#ifdef HAVE_FDATASYNC
	/* write, fdatasync */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		write(tmpfile, strout, 8192);
		fdatasync(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\twrite, fdatasync       ");
	print_elapse(start_t, elapse_t);
#else
	printf("\t(fdatasync unavailable)");
#endif
	printf("\n");

	/* write, fsync, close */
	if ((tmpfile = open(FSYNC_FILENAME, O_RDWR)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	gettimeofday(&start_t, NULL);
	for (i = 0; i < loops; i++)
	{
		write(tmpfile, strout, 8192);
		write(tmpfile, strout, 8192);
		fsync(tmpfile);
	}
	gettimeofday(&elapse_t, NULL);
	close(tmpfile);
	printf("\twrite, fsync,          ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	unlink(FSYNC_FILENAME);

	return 0;
}

void
print_elapse(struct timeval start_t, struct timeval elapse_t)
{
	if (elapse_t.tv_usec < start_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}

	printf("%3ld.%06ld", (long) (elapse_t.tv_sec - start_t.tv_sec),
		   (long) (elapse_t.tv_usec - start_t.tv_usec));
}

void
die(char *str)
{
	fprintf(stderr, "%s", str);
	exit(1);
}
