/*-------------------------------------------------------------------------
 *
 * test_thread_funcs.c
 *      libc thread test program
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	$Header: /cvsroot/pgsql/src/tools/thread/Attic/thread_test.c,v 1.3.2.2 2003/12/19 23:29:29 momjian Exp $
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

void func_call_1(void);
void func_call_2(void);

#ifndef HAVE_GETADDRINFO
struct hostent *hostent_p1;
struct hostent *hostent_p2;
#endif

struct passwd *passwd_p1;
struct passwd *passwd_p2;

char *strerror_p1;
char *strerror_p2;

int main(int argc, char *argv[])
{
	pthread_t		thread1,
					thread2;

	if (argc > 1)
	{
			fprintf(stderr, "Usage: %s\n", argv[0]);
			return 1;
	}

	printf("\
Make sure you have added any needed 'THREAD_CPPFLAGS' and 'THREAD_LIBS'\n\
defines to your template/$port file before compiling this program.\n\n"
);
	pthread_create(&thread1, NULL, (void * (*)(void *)) func_call_1, NULL);
	pthread_create(&thread2, NULL, (void * (*)(void *)) func_call_2, NULL);
	pthread_join(thread1, NULL);
	pthread_join(thread2, NULL);

#ifndef HAVE_GETADDRINFO
	if (hostent_p1 == hostent_p2)
		printf("Your gethostbyname() is _not_ thread-safe\n");
#endif
	if (passwd_p1 == passwd_p2)
		printf("Your getpwuid() is _not_ thread-safe\n");
	if (strerror_p1 == strerror_p2)
		printf("Your strerror() is _not_ thread-safe\n");

	if (
#ifndef HAVE_GETADDRINFO
		hostent_p1 != hostent_p2 &&
#endif
		passwd_p1 != passwd_p2 &&
		strerror_p1 != strerror_p2)
	{
		printf("All your non-*_r functions are thread-safe.\n");
		printf("Add this to your template/$port file:\n\n");
		printf("NEED_REENTRANT_FUNCS=no\n");
	}
	else
	{
		printf("Not all non-*_r functions are thread-safe.\n");
		printf("Add this to your template/$port file:\n\n");
		printf("NEED_REENTRANT_FUNCS=yes\n");
	}

	return 0;
}

void func_call_1(void) {
	void *p;

#ifndef HAVE_GETADDRINFO
	hostent_p1 = gethostbyname("yahoo.com");
	p = gethostbyname("slashdot.org");
	if (hostent_p1 != p)
	{
		printf("Your gethostbyname() changes the static memory area between calls\n");
		hostent_p1 = NULL;	/* force thread-safe failure report */
	}
#endif

	passwd_p1 = getpwuid(0);
	p = getpwuid(1);
	if (passwd_p1 != p)
	{
		printf("Your getpwuid() changes the static memory area between calls\n");
		passwd_p1 = NULL;	/* force thread-safe failure report */
	}

	strerror_p1 = strerror(EACCES);
	/*
	 *	If strerror() uses sys_errlist, the pointer might change for different
	 *	errno values, so we don't check to see if it varies within the thread.
	 */
}


void func_call_2(void) {
	void *p;

#ifndef HAVE_GETADDRINFO
	hostent_p2 = gethostbyname("google.com");
	p = gethostbyname("postgresql.org");
	if (hostent_p2 != p)
	{
		printf("Your gethostbyname() changes the static memory area between calls\n");
		hostent_p2 = NULL;	/* force thread-safe failure report */
	}
#endif

	passwd_p2 = getpwuid(2);
	p = getpwuid(3);
	if (passwd_p2 != p)
	{
		printf("Your getpwuid() changes the static memory area between calls\n");
		passwd_p2 = NULL;	/* force thread-safe failure report */
	}

	strerror_p2 = strerror(EINVAL);
	/*
	 *	If strerror() uses sys_errlist, the pointer might change for different
	 *	errno values, so we don't check to see if it varies within the thread.
	 */
}
