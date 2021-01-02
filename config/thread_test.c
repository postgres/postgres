/*-------------------------------------------------------------------------
 *
 * thread_test.c
 *		libc threading test program
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	config/thread_test.c
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

/* We cannot use c.h, as port.h will not exist yet */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

/* CYGWIN requires this for MAXHOSTNAMELEN */
#ifdef __CYGWIN__
#include <sys/param.h>
#endif

#ifdef WIN32
#define MAXHOSTNAMELEN 63
#include <winsock2.h>
#endif

/* Test for POSIX.1c 2-arg sigwait() and fail on single-arg version */
#include <signal.h>
int			sigwait(const sigset_t *set, int *sig);


#define		TEMP_FILENAME_1 "thread_test.1"
#define		TEMP_FILENAME_2 "thread_test.2"

static void func_call_1(void);
static void func_call_2(void);

static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int thread1_done = 0;
static volatile int thread2_done = 0;

static volatile int errno1_set = 0;
static volatile int errno2_set = 0;

#ifndef HAVE_STRERROR_R
static char *strerror_p1;
static char *strerror_p2;
static int	strerror_threadsafe = 0;
#endif

#if !defined(WIN32) && !defined(HAVE_GETPWUID_R)
static struct passwd *passwd_p1;
static struct passwd *passwd_p2;
static int	getpwuid_threadsafe = 0;
#endif

#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETHOSTBYNAME_R)
static struct hostent *hostent_p1;
static struct hostent *hostent_p2;
static char myhostname[MAXHOSTNAMELEN];
static int	gethostbyname_threadsafe = 0;
#endif

static int	platform_is_threadsafe = 1;

int
main(int argc, char *argv[])
{
	pthread_t	thread1,
				thread2;
	int			rc;

#ifdef WIN32
	WSADATA		wsaData;
	int			err;
#endif

	if (argc > 1)
	{
		fprintf(stderr, "Usage: %s\n", argv[0]);
		return 1;
	}

	/* Send stdout to 'config.log' */
	close(1);
	dup(5);

#ifdef WIN32
	err = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (err != 0)
	{
		fprintf(stderr, "Cannot start the network subsystem - %d**\nexiting\n", err);
		exit(1);
	}
#endif

#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETHOSTBYNAME_R)
	if (gethostname(myhostname, MAXHOSTNAMELEN) != 0)
	{
		fprintf(stderr, "Cannot get local hostname **\nexiting\n");
		exit(1);
	}
#endif

	/* Hold lock until we are ready for the child threads to exit. */
	pthread_mutex_lock(&init_mutex);

	rc = pthread_create(&thread1, NULL, (void *(*) (void *)) func_call_1, NULL);
	if (rc != 0)
	{
		fprintf(stderr, "Failed to create thread 1: %s **\nexiting\n",
				strerror(errno));
		exit(1);
	}
	rc = pthread_create(&thread2, NULL, (void *(*) (void *)) func_call_2, NULL);
	if (rc != 0)
	{
		/*
		 * strerror() might not be thread-safe, and we already spawned thread
		 * 1 that uses it, so avoid using it.
		 */
		fprintf(stderr, "Failed to create thread 2 **\nexiting\n");
		exit(1);
	}

	while (thread1_done == 0 || thread2_done == 0)
		sched_yield();			/* if this is a portability problem, remove it */

	/* Test things while we have thread-local storage */

	/* If we got here, we didn't exit() from a thread */
#ifdef WIN32
	printf("Your GetLastError() is thread-safe.\n");
#else
	printf("Your errno is thread-safe.\n");
#endif

#ifndef HAVE_STRERROR_R
	if (strerror_p1 != strerror_p2)
		strerror_threadsafe = 1;
#endif

#if !defined(WIN32) && !defined(HAVE_GETPWUID_R)
	if (passwd_p1 != passwd_p2)
		getpwuid_threadsafe = 1;
#endif

#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETHOSTBYNAME_R)
	if (hostent_p1 != hostent_p2)
		gethostbyname_threadsafe = 1;
#endif

	/* close down threads */

	pthread_mutex_unlock(&init_mutex);	/* let children exit  */

	pthread_join(thread1, NULL);	/* clean up children */
	pthread_join(thread2, NULL);

	/* report results */

#ifdef HAVE_STRERROR_R
	printf("Your system has strerror_r(); it does not need strerror().\n");
#else
	printf("Your system uses strerror() which is ");
	if (strerror_threadsafe)
		printf("thread-safe.\n");
	else
	{
		printf("not thread-safe. **\n");
		platform_is_threadsafe = 0;
	}
#endif

#ifdef WIN32
	printf("getpwuid_r()/getpwuid() are not applicable to Win32 platforms.\n");
#elif defined(HAVE_GETPWUID_R)
	printf("Your system has getpwuid_r();  it does not need getpwuid().\n");
#else
	printf("Your system uses getpwuid() which is ");
	if (getpwuid_threadsafe)
		printf("thread-safe.\n");
	else
	{
		printf("not thread-safe. **\n");
		platform_is_threadsafe = 0;
	}
#endif

#ifdef HAVE_GETADDRINFO
	printf("Your system has getaddrinfo();  it does not need gethostbyname()\n"
		   "  or gethostbyname_r().\n");
#elif defined(HAVE_GETHOSTBYNAME_R)
	printf("Your system has gethostbyname_r();  it does not need gethostbyname().\n");
#else
	printf("Your system uses gethostbyname which is ");
	if (gethostbyname_threadsafe)
		printf("thread-safe.\n");
	else
	{
		printf("not thread-safe. **\n");
		platform_is_threadsafe = 0;
	}
#endif

	if (platform_is_threadsafe)
	{
		printf("\nYour platform is thread-safe.\n");
		return 0;
	}
	else
	{
		printf("\n** YOUR PLATFORM IS NOT THREAD-SAFE. **\n");
		return 1;
	}
}

/*
 * func_call_1
 */
static void
func_call_1(void)
{
#if !defined(HAVE_GETPWUID_R) || \
	(!defined(HAVE_GETADDRINFO) && \
	 !defined(HAVE_GETHOSTBYNAME_R))
	void	   *p;
#endif
#ifdef WIN32
	HANDLE		h1;
#else
	int			fd;
#endif

	unlink(TEMP_FILENAME_1);

	/* Set errno = EEXIST */

	/* create, then try to fail on exclusive create open */

	/*
	 * It would be great to check errno here but if errno is not thread-safe
	 * we might get a value from the other thread and mis-report the cause of
	 * the failure.
	 */
#ifdef WIN32
	if ((h1 = CreateFile(TEMP_FILENAME_1, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL)) ==
		INVALID_HANDLE_VALUE)
#else
	if ((fd = open(TEMP_FILENAME_1, O_RDWR | O_CREAT, 0600)) < 0)
#endif
	{
		fprintf(stderr, "Could not create file %s in current directory\n",
				TEMP_FILENAME_1);
		exit(1);
	}

#ifdef WIN32
	if (CreateFile(TEMP_FILENAME_1, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL)
		!= INVALID_HANDLE_VALUE)
#else
	if (open(TEMP_FILENAME_1, O_RDWR | O_CREAT | O_EXCL, 0600) >= 0)
#endif
	{
		fprintf(stderr,
				"Could not generate failure for exclusive file create of %s in current directory **\nexiting\n",
				TEMP_FILENAME_1);
		exit(1);
	}

	/*
	 * Wait for other thread to set errno. We can't use thread-specific
	 * locking here because it might affect errno.
	 */
	errno1_set = 1;
	while (errno2_set == 0)
		sched_yield();

#ifdef WIN32
	if (GetLastError() != ERROR_FILE_EXISTS)
#else
	if (errno != EEXIST)
#endif
	{
#ifdef WIN32
		fprintf(stderr, "GetLastError() not thread-safe **\nexiting\n");
#else
		fprintf(stderr, "errno not thread-safe **\nexiting\n");
#endif
		unlink(TEMP_FILENAME_1);
		exit(1);
	}

#ifdef WIN32
	CloseHandle(h1);
#else
	close(fd);
#endif
	unlink(TEMP_FILENAME_1);

#ifndef HAVE_STRERROR_R

	/*
	 * If strerror() uses sys_errlist, the pointer might change for different
	 * errno values, so we don't check to see if it varies within the thread.
	 */
	strerror_p1 = strerror(EACCES);
#endif

#if !defined(WIN32) && !defined(HAVE_GETPWUID_R)
	passwd_p1 = getpwuid(0);
	p = getpwuid(1);
	if (passwd_p1 != p)
	{
		printf("Your getpwuid() changes the static memory area between calls\n");
		passwd_p1 = NULL;		/* force thread-safe failure report */
	}
#endif

#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETHOSTBYNAME_R)
	/* threads do this in opposite order */
	hostent_p1 = gethostbyname(myhostname);
	p = gethostbyname("localhost");
	if (hostent_p1 != p)
	{
		printf("Your gethostbyname() changes the static memory area between calls\n");
		hostent_p1 = NULL;		/* force thread-safe failure report */
	}
#endif

	thread1_done = 1;
	pthread_mutex_lock(&init_mutex);	/* wait for parent to test */
	pthread_mutex_unlock(&init_mutex);
}


/*
 * func_call_2
 */
static void
func_call_2(void)
{
#if !defined(HAVE_GETPWUID_R) || \
	(!defined(HAVE_GETADDRINFO) && \
	 !defined(HAVE_GETHOSTBYNAME_R))
	void	   *p;
#endif

	unlink(TEMP_FILENAME_2);

	/* Set errno = ENOENT */

	/* This will fail, but we can't check errno yet */
	if (unlink(TEMP_FILENAME_2) != -1)
	{
		fprintf(stderr,
				"Could not generate failure for unlink of %s in current directory **\nexiting\n",
				TEMP_FILENAME_2);
		exit(1);
	}

	/*
	 * Wait for other thread to set errno. We can't use thread-specific
	 * locking here because it might affect errno.
	 */
	errno2_set = 1;
	while (errno1_set == 0)
		sched_yield();

#ifdef WIN32
	if (GetLastError() != ENOENT)
#else
	if (errno != ENOENT)
#endif
	{
#ifdef WIN32
		fprintf(stderr, "GetLastError() not thread-safe **\nexiting\n");
#else
		fprintf(stderr, "errno not thread-safe **\nexiting\n");
#endif
		exit(1);
	}

#ifndef HAVE_STRERROR_R

	/*
	 * If strerror() uses sys_errlist, the pointer might change for different
	 * errno values, so we don't check to see if it varies within the thread.
	 */
	strerror_p2 = strerror(EINVAL);
#endif

#if !defined(WIN32) && !defined(HAVE_GETPWUID_R)
	passwd_p2 = getpwuid(2);
	p = getpwuid(3);
	if (passwd_p2 != p)
	{
		printf("Your getpwuid() changes the static memory area between calls\n");
		passwd_p2 = NULL;		/* force thread-safe failure report */
	}
#endif

#if !defined(HAVE_GETADDRINFO) && !defined(HAVE_GETHOSTBYNAME_R)
	/* threads do this in opposite order */
	hostent_p2 = gethostbyname("localhost");
	p = gethostbyname(myhostname);
	if (hostent_p2 != p)
	{
		printf("Your gethostbyname() changes the static memory area between calls\n");
		hostent_p2 = NULL;		/* force thread-safe failure report */
	}
#endif

	thread2_done = 1;
	pthread_mutex_lock(&init_mutex);	/* wait for parent to test */
	pthread_mutex_unlock(&init_mutex);
}
