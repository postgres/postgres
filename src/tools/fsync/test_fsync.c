/*
 *	test_fsync.c
 *		tests if fsync can be done from another process than the original write
 */

#include "../../include/pg_config.h"

#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

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

void die(char *str);
void print_elapse(struct timeval start_t, struct timeval elapse_t);

int main(int argc, char *argv[])
{
	struct timeval start_t;
	struct timeval elapse_t;
	int tmpfile;
	char *strout = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	printf("Simple write timing:\n");
	/* write only */	
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	close(tmpfile);		
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("write                  ");
	print_elapse(start_t, elapse_t);
	printf("\n\n");

	printf("Compare fsync before and after write's close:\n");
	/* write, fsync, close */
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	fsync(tmpfile);
	close(tmpfile);		
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("write, fsync, close    ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	/* write, close, fsync */
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	close(tmpfile);
	/* reopen file */
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	fsync(tmpfile);
	close(tmpfile);		
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("write, close, fsync    ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	printf("\nTest file sync methods:\n");

#ifdef OPEN_DATASYNC_FLAG
	/* open_dsync, write */
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT | O_DSYNC, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	close(tmpfile);
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("open o_dsync, write    ");
	print_elapse(start_t, elapse_t);
#else
	printf("o_dsync unavailable    ");
#endif
	printf("\n");

	/* open_fsync, write */
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT | OPEN_SYNC_FLAG, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	close(tmpfile);
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("open o_fsync, write    ");
	print_elapse(start_t, elapse_t);
	printf("\n");

#ifdef HAVE_FDATASYNC
	/* write, fdatasync */
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	fdatasync(tmpfile);
	close(tmpfile);
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("write, fdatasync       ");
	print_elapse(start_t, elapse_t);
#else
	printf("fdatasync unavailable  ");
#endif
	printf("\n");

	/* write, fsync, close */
	gettimeofday(&start_t, NULL);
	if ((tmpfile = open("/var/tmp/test_fsync.out", O_RDWR | O_CREAT, 0600)) == -1)
		die("can't open /var/tmp/test_fsync.out");
	write(tmpfile, &strout, 200);
	fsync(tmpfile);
	close(tmpfile);
	gettimeofday(&elapse_t, NULL);
	unlink("/var/tmp/test_fsync.out");
	printf("write, fsync,          ");
	print_elapse(start_t, elapse_t);
	printf("\n");

	return 0;
}

void print_elapse(struct timeval start_t, struct timeval elapse_t)
{
	if (elapse_t.tv_usec < start_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}

	printf("%ld.%06ld", (long) (elapse_t.tv_sec - start_t.tv_sec),
					 (long) (elapse_t.tv_usec - start_t.tv_usec));
}

void die(char *str)
{
	fprintf(stderr, "%s", str);
	exit(1);
}
