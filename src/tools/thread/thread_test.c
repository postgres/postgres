/*-------------------------------------------------------------------------
 *
 * test_thread_funcs.c
 *      libc thread test program
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$PostgreSQL: pgsql/src/tools/thread/thread_test.c,v 1.11 2004/04/05 01:27:58 momjian Exp $
 *
 *	This program tests to see if your standard libc functions use
 *	pthread_setspecific()/pthread_getspecific() to be thread-safe.
 *	See src/port/thread.c for more details.
 *
 *	This program first tests to see if each function returns a constant
 *	memory pointer within the same thread, then, assuming it does, tests
 *	to see if the pointers are different for different threads.  If they
 *	are, the function is thread-safe.
 *
 *-------------------------------------------------------------------------
 */

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <errno.h>

void func_call_1(void);
void func_call_2(void);

char myhostname[MAXHOSTNAMELEN];

int errno1_set = 0;
int errno2_set = 0;

char *strerror_p1;
char *strerror_p2;

struct passwd *passwd_p1;
struct passwd *passwd_p2;

struct hostent *hostent_p1;
struct hostent *hostent_p2;

pthread_mutex_t singlethread_lock1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t singlethread_lock2 = PTHREAD_MUTEX_INITIALIZER;
                            
int main(int argc, char *argv[])
{
	pthread_t		thread1,
					thread2;

	if (argc > 1)
	{
			fprintf(stderr, "Usage: %s\n", argv[0]);
			return 1;
	}

	if (gethostname(myhostname, MAXHOSTNAMELEN) != 0)
	{
			fprintf(stderr, "can not get local hostname, exiting\n");
			exit(1);
	}

	printf("\
Make sure you have added any needed 'THREAD_CPPFLAGS' and 'THREAD_LIBS'\n\
defines to your template/$port file before compiling this program.\n\n"
);
	pthread_create(&thread1, NULL, (void * (*)(void *)) func_call_1, NULL);
	pthread_create(&thread2, NULL, (void * (*)(void *)) func_call_2, NULL);
	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);

	printf("Add this to your template/$port file:\n\n");

	if (strerror_p1 != strerror_p2)
		printf("STRERROR_THREADSAFE=yes\n");
	else
		printf("STRERROR_THREADSAFE=no\n");

	if (passwd_p1 != passwd_p2)
		printf("GETPWUID_THREADSAFE=yes\n");
	else
		printf("GETPWUID_THREADSAFE=no\n");

	if (hostent_p1 != hostent_p2)
		printf("GETHOSTBYNAME_THREADSAFE=yes\n");
	else
		printf("GETHOSTBYNAME_THREADSAFE=no\n");
	
	return 0;
}

void func_call_1(void) {
	void *p;
	
	unlink("/tmp/thread_test.1");
	/* create, then try to fail on exclusive create open */
	if (open("/tmp/thread_test.1", O_RDWR | O_CREAT, 0600) < 0 ||
		open("/tmp/thread_test.1", O_RDWR | O_CREAT | O_EXCL, 0600) >= 0)
	{
			fprintf(stderr, "Could not create file in /tmp or\n");
			fprintf(stderr, "could not generate failure for create file in /tmp, exiting\n");
			exit(1);
	}
	/* wait for other thread to set errno */
	errno1_set = 1;
	while (errno2_set == 0)
		getpid();	/* force system call */
	if (errno != EEXIST)
	{
			fprintf(stderr, "errno not thread-safe; exiting\n");
			unlink("/tmp/thread_test.1");
			exit(1);
	}
	unlink("/tmp/thread_test.1");
	
	strerror_p1 = strerror(EACCES);
	/*
	 *	If strerror() uses sys_errlist, the pointer might change for different
	 *	errno values, so we don't check to see if it varies within the thread.
	 */

	passwd_p1 = getpwuid(0);
	p = getpwuid(1);
	if (passwd_p1 != p)
	{
		printf("Your getpwuid() changes the static memory area between calls\n");
		passwd_p1 = NULL;	/* force thread-safe failure report */
	}

	/* threads do this in opposite order */
	hostent_p1 = gethostbyname(myhostname);
	p = gethostbyname("localhost");
	if (hostent_p1 != p)
	{
		printf("Your gethostbyname() changes the static memory area between calls\n");
		hostent_p1 = NULL;	/* force thread-safe failure report */
	}
}


void func_call_2(void) {
	void *p;

	unlink("/tmp/thread_test.2");
	/* open non-existant file */
	if (open("/tmp/thread_test.2", O_RDONLY, 0600) >= 0)
	{
			fprintf(stderr, "Read-only open succeeded without create, exiting\n");
			exit(1);
	}
	/* wait for other thread to set errno */
	errno2_set = 1;
	while (errno1_set == 0)
		getpid();	/* force system call */
	if (errno != ENOENT)
	{
			fprintf(stderr, "errno not thread-safe; exiting\n");
			unlink("/tmp/thread_test.A");
			exit(1);
	}
	unlink("/tmp/thread_test.2");
	
	strerror_p2 = strerror(EINVAL);
	/*
	 *	If strerror() uses sys_errlist, the pointer might change for different
	 *	errno values, so we don't check to see if it varies within the thread.
	 */

	passwd_p2 = getpwuid(2);
	p = getpwuid(3);
	if (passwd_p2 != p)
	{
		printf("Your getpwuid() changes the static memory area between calls\n");
		passwd_p2 = NULL;	/* force thread-safe failure report */
	}

	/* threads do this in opposite order */
	hostent_p2 = gethostbyname("localhost");
	p = gethostbyname(myhostname);
	if (hostent_p2 != p)
	{
		printf("Your gethostbyname() changes the static memory area between calls\n");
		hostent_p2 = NULL;	/* force thread-safe failure report */
	}
}
