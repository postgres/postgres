/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.	Frontend programs send a startup message
 *	  to the Postmaster and the postmaster uses the info in the
 *	  message to setup a backend process.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/postmaster/postmaster.c,v 1.203 2001/01/24 19:43:04 momjian Exp $
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up a few shared memory data structures
 *		for the backends.  It should at the very least initialize the
 *		lock manager.
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends and will have to lock
 *		the shared memory it accesses.	The Postmaster should never block
 *		on messages from clients.
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an emergency
 *		exit and/or core dump.
 *
 * Communication:
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/param.h>
/* moved here to prevent double define */
#include <netdb.h>
#include <limits.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "catalog/pg_database.h"
#include "commands/async.h"
#include "lib/dllist.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "libpq/pqcomm.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "access/xlog.h"
#include "tcop/tcopprot.h"
#include "utils/exc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "bootstrap/bootstrap.h"

#define INVALID_SOCK	(-1)
#define ARGV_SIZE	64

#ifdef HAVE_SIGPROCMASK
sigset_t	UnBlockSig,
			BlockSig;

#else
int			UnBlockSig,
			BlockSig;

#endif

/*
 * Info for garbage collection.  Whenever a process dies, the Postmaster
 * cleans up after it.	Currently, NO information is required for cleanup,
 * but I left this structure around in case that changed.
 */
typedef struct bkend
{
	int			pid;			/* process id of backend */
	long		cancel_key;		/* cancel key for cancels for this backend */
} Backend;

/* list of active backends.  For garbage collection only now. */
static Dllist *BackendList;

/* list of ports associated with still open, but incomplete connections */
static Dllist *PortList;

/* The socket number we are listening for connections on */
int PostPortNumber;
char *UnixSocketDir;
char *VirtualHost;

/*
 * MaxBackends is the actual limit on the number of backends we will
 * start. The default is established by configure, but it can be
 * readjusted from 1..MAXBACKENDS with the postmaster -N switch. Note
 * that a larger MaxBackends value will increase the size of the shared
 * memory area as well as cause the postmaster to grab more kernel
 * semaphores, even if you never actually use that many backends.
 */
int	MaxBackends = DEF_MAXBACKENDS;


static char *progname = (char *) NULL;
static char **real_argv;
static int	real_argc;

static time_t tnow;

/* flag to indicate that SIGHUP arrived during server loop */
static volatile bool got_SIGHUP = false;

/*
 * Default Values
 */
static int	ServerSock_INET = INVALID_SOCK;		/* stream socket server */

#ifdef HAVE_UNIX_SOCKETS
static int	ServerSock_UNIX = INVALID_SOCK;		/* stream socket server */
#endif

#ifdef USE_SSL
static SSL_CTX *SSL_context = NULL;		/* Global SSL context */
#endif

/*
 * Set by the -o option
 */
static char ExtraOptions[MAXPGPATH];

/*
 * These globals control the behavior of the postmaster in case some
 * backend dumps core.	Normally, it kills all peers of the dead backend
 * and reinitializes shared memory.  By specifying -s or -n, we can have
 * the postmaster stop (rather than kill) peers and not reinitialize
 * shared data structures.
 */
static bool Reinit = true;
static int	SendStop = false;

bool NetServer = false;	/* listen on TCP/IP */
bool EnableSSL = false;
bool SilentMode = false;	/* silent mode (-S) */

int				CheckPointTimeout = 300;

static pid_t	StartupPID = 0,
				ShutdownPID = 0,
				CheckPointPID = 0;
static time_t	checkpointed = 0;

#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2

static int	Shutdown = NoShutdown;

static bool FatalError = false;	/* T if recovering from backend crash */

/*
 * State for assigning random salts and cancel keys.
 * Also, the global MyCancelKey passes the cancel key assigned to a given
 * backend from the postmaster to that backend (via fork).
 */

static unsigned int random_seed = 0;

extern char *optarg;
extern int	optind,
			opterr;

extern void GetRedoRecPtr(void);

/*
 * postmaster.c - function prototypes
 */
static void pmdaemonize(int argc, char *argv[]);
static Port *ConnCreate(int serverFd);
static void ConnFree(Port *port);
static void reset_shared(unsigned short port);
static void SIGHUP_handler(SIGNAL_ARGS);
static void pmdie(SIGNAL_ARGS);
static void reaper(SIGNAL_ARGS);
static void dumpstatus(SIGNAL_ARGS);
static void CleanupProc(int pid, int exitstatus);
static int	DoBackend(Port *port);
static void ExitPostmaster(int status);
static void usage(const char *);
static int	ServerLoop(void);
static int	BackendStartup(Port *port);
static int	readStartupPacket(void *arg, PacketLen len, void *pkt);
static int	processCancelRequest(Port *port, PacketLen len, void *pkt);
static int	initMasks(fd_set *rmask, fd_set *wmask);
static char *canAcceptConnections(void);
static long PostmasterRandom(void);
static void RandomSalt(char *salt);
static void SignalChildren(int signal);
static int	CountChildren(void);
static bool CreateOptsFile(int argc, char *argv[]);

static pid_t SSDataBase(int xlop);

#define StartupDataBase()		SSDataBase(BS_XLOG_STARTUP)
#define CheckPointDataBase()	SSDataBase(BS_XLOG_CHECKPOINT)
#define ShutdownDataBase()		SSDataBase(BS_XLOG_SHUTDOWN)

#ifdef USE_SSL
static void InitSSL(void);

#endif

#ifdef CYR_RECODE
extern void GetCharSetByHost(char *, int, char *);

#endif


static void
checkDataDir(const char *checkdir)
{
	char		path[MAXPGPATH];
	FILE	   *fp;

	if (checkdir == NULL)
	{
		fprintf(stderr, "%s does not know where to find the database system "
				"data.  You must specify the directory that contains the "
				"database system either by specifying the -D invocation "
				"option or by setting the PGDATA environment variable.\n\n",
				progname);
		ExitPostmaster(2);
	}

	snprintf(path, sizeof(path), "%s%cglobal%cpg_control",
			 checkdir, SEP_CHAR, SEP_CHAR);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		fprintf(stderr, "%s does not find the database system."
				"\n\tExpected to find it in the PGDATA directory \"%s\","
				"\n\tbut unable to open file \"%s\": %s\n\n",
				progname, checkdir, path, strerror(errno));
		ExitPostmaster(2);
	}

	FreeFile(fp);

	ValidatePgVersion(checkdir);
}


int
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char		original_extraoptions[MAXPGPATH];
	char       *potential_DataDir = NULL;

	IsUnderPostmaster = true;	/* so that backends know this */

	*original_extraoptions = '\0';

	progname = argv[0];
	real_argv = argv;
	real_argc = argc;

	/*
	 * Catch standard options before doing much else.  This even works
	 * on systems without getopt_long.
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help")==0 || strcmp(argv[1], "-?")==0)
		{
			usage(progname);
			ExitPostmaster(0);
		}
		if (strcmp(argv[1], "--version")==0 || strcmp(argv[1], "-V")==0)
		{
			puts("postmaster (PostgreSQL) " PG_VERSION);
			ExitPostmaster(0);
		}
	}		


	/*
	 * for security, no dir or file created can be group or other
	 * accessible
	 */
	umask((mode_t) 0077);

	MyProcPid = getpid();

	/*
	 * Fire up essential subsystems: error and memory management
	 */
	EnableExceptionHandling(true);
	MemoryContextInit();

	/*
	 * By default, palloc() requests in the postmaster will be allocated
	 * in the PostmasterContext, which is space that can be recycled by
	 * backends.  Allocated data that needs to be available to backends
	 * should be allocated in TopMemoryContext.
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(PostmasterContext);

	/*
	 * Options setup
	 */
	potential_DataDir = getenv("PGDATA"); /* default value */

	ResetAllOptions();

	/*
	 * First we must scan for a -D argument to get the data dir. Then
	 * read the config file. Finally, scan all the other arguments.
	 * (Command line switches override config file.)
	 *
	 * Note: The two lists of options must be exactly the same, even
	 * though perhaps the first one would only have to be "D:" with
	 * opterr turned off. But some versions of getopt (notably GNU)
	 * are going to arbitrarily permute some "non-options" (according
	 * to the local world view) which will result in some switches
	 * being associated with the wrong argument. Death and destruction
	 * will occur.
	 */
	opterr = 1;
	while ((opt = getopt(argc, argv, "A:a:B:b:c:D:d:Fh:ik:lm:MN:no:p:Ss-:")) != EOF)
	{
		switch(opt)
		{
			case 'D':
				potential_DataDir = optarg;
				break;

			case '?':
				fprintf(stderr, "Try '%s --help' for more information.\n", progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Non-option switch arguments don't exist.
	 */
	if (optind < argc)
	{
		fprintf(stderr, "%s: invalid argument -- %s\n", progname, argv[optind]);
		fprintf(stderr, "Try '%s --help' for more information.\n", progname);
		ExitPostmaster(1);
	}

	checkDataDir(potential_DataDir);	/* issues error messages */
	SetDataDir(potential_DataDir);

	ProcessConfigFile(PGC_POSTMASTER);

	IgnoreSystemIndexes(false);

	optind = 1; /* start over */
#ifdef HAVE_INT_OPTRESET
	optreset = 1;
#endif
	while ((opt = getopt(argc, argv, "A:a:B:b:c:D:d:Fh:ik:lm:MN:no:p:Ss-:")) != EOF)
	{
		switch (opt)
		{
			case 'A':
#ifndef USE_ASSERT_CHECKING
				fprintf(stderr, "Assert checking is not compiled in\n");
#else
				assert_enabled = atoi(optarg);
#endif
				break;
			case 'a':
				/* Can no longer set authentication method. */
				break;
			case 'B':
				NBuffers = atoi(optarg);
				break;
			case 'b':
				/* Can no longer set the backend executable file to use. */
				break;
			case 'D':
				/* already done above */
				break;
			case 'd':

				/*
				 * Turn on debugging for the postmaster and the backend
				 * servers descended from it.
				 */
				DebugLvl = atoi(optarg);
				break;
			case 'F':
				enableFsync = false;
				break;
			case 'h':
				VirtualHost = optarg;
				break;
			case 'i':
				NetServer = true;
				break;
			case 'k':
				UnixSocketDir = optarg;
				break;
#ifdef USE_SSL
			case 'l':
			        EnableSSL = true;
				break;
#endif
			case 'm':
				/* Multiplexed backends no longer supported. */
				break;
			case 'M':

				/*
				 * ignore this flag.  This may be passed in because the
				 * program was run as 'postgres -M' instead of
				 * 'postmaster'
				 */
				break;
			case 'N':

				/*
				 * The max number of backends to start. Can't set to less
				 * than 1 or more than compiled-in limit.
				 */
				MaxBackends = atoi(optarg);
				if (MaxBackends < 1)
					MaxBackends = 1;
				if (MaxBackends > MAXBACKENDS)
					MaxBackends = MAXBACKENDS;
				break;
			case 'n':
				/* Don't reinit shared mem after abnormal exit */
				Reinit = false;
				break;
			case 'o':

				/*
				 * Other options to pass to the backend on the command
				 * line -- useful only for debugging.
				 */
				strcat(ExtraOptions, " ");
				strcat(ExtraOptions, optarg);
				strcpy(original_extraoptions, optarg);
				break;
			case 'p':
				PostPortNumber = atoi(optarg);
				break;
			case 'S':

				/*
				 * Start in 'S'ilent mode (disassociate from controlling
				 * tty). You may also think of this as 'S'ysV mode since
				 * it's most badly needed on SysV-derived systems like
				 * SVR4 and HP-UX.
				 */
				SilentMode = true;
				break;
			case 's':

				/*
				 * In the event that some backend dumps core, send
				 * SIGSTOP, rather than SIGUSR1, to all its peers.	This
				 * lets the wily post_hacker collect core dumps from
				 * everyone.
				 */
				SendStop = true;
				break;
			case 'c':
			case '-':
			{
				char *name, *value;

				ParseLongOption(optarg, &name, &value);
				if (!value)
				{
					if (opt == '-')
						elog(ERROR, "--%s requires argument", optarg);
					else
						elog(ERROR, "-c %s requires argument", optarg);
				}

				SetConfigOption(name, value, PGC_POSTMASTER);
				free(name);
				if (value)
					free(value);
				break;
			}

			default:
				/* shouldn't get here */
				fprintf(stderr, "Try '%s --help' for more information.\n", progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Check for invalid combinations of switches
	 */
	if (NBuffers < 2 * MaxBackends || NBuffers < 16)
	{

		/*
		 * Do not accept -B so small that backends are likely to starve
		 * for lack of buffers.  The specific choices here are somewhat
		 * arbitrary.
		 */
		fprintf(stderr, "%s: The number of buffers (-B) must be at least twice the number of allowed connections (-N) and at least 16.\n",
				progname);
		ExitPostmaster(1);
	}

	if (DebugLvl > 2)
	{
		extern char **environ;
		char	  **p;

		fprintf(stderr, "%s: PostmasterMain: initial environ dump:\n",
				progname);
		fprintf(stderr, "-----------------------------------------\n");
		for (p = environ; *p; ++p)
			fprintf(stderr, "\t%s\n", *p);
		fprintf(stderr, "-----------------------------------------\n");
	}

	/*
	 * Fork away from controlling terminal, if -S specified.
	 *
	 * Must do this before we grab any interlock files, else the interlocks
	 * will show the wrong PID.
	 */
	if (SilentMode)
		pmdaemonize(argc, argv);

	/*
	 * Create lockfile for data directory.
	 *
	 * We want to do this before we try to grab the input sockets, because
	 * the data directory interlock is more reliable than the socket-file
	 * interlock (thanks to whoever decided to put socket files in /tmp :-().
	 * For the same reason, it's best to grab the TCP socket before the
	 * Unix socket.
	 */
	if (! CreateDataDirLockFile(DataDir, true))
		ExitPostmaster(1);

	/*
	 * Establish input sockets.
	 */
#ifdef USE_SSL
	if (EnableSSL && !NetServer)
	{
		fprintf(stderr, "%s: For SSL, TCP/IP connections must be enabled. See -? for help.\n",
				progname);
		ExitPostmaster(1);
	}
	if (EnableSSL)
	        InitSSL();
#endif

	if (NetServer)
	{
		status = StreamServerPort(AF_INET, VirtualHost,
						(unsigned short) PostPortNumber, UnixSocketDir,
						&ServerSock_INET);
		if (status != STATUS_OK)
		{
			fprintf(stderr, "%s: cannot create INET stream port\n",
					progname);
			ExitPostmaster(1);
		}
	}

#ifdef HAVE_UNIX_SOCKETS
	status = StreamServerPort(AF_UNIX, VirtualHost,
						(unsigned short) PostPortNumber, UnixSocketDir, 
						&ServerSock_UNIX);
	if (status != STATUS_OK)
	{
		fprintf(stderr, "%s: cannot create UNIX stream port\n",
				progname);
		ExitPostmaster(1);
	}
#endif

	XLOGPathInit();

	/*
	 * Set up shared memory and semaphores.
	 */
	reset_shared(PostPortNumber);

	/*
	 * Initialize the list of active backends.	This list is only used for
	 * garbage collecting the backend processes.
	 */
	BackendList = DLNewList();
	PortList = DLNewList();

	/*
	 * Record postmaster options.  We delay this till now to avoid recording
	 * bogus options (eg, NBuffers too high for available memory).
	 */
	if (!CreateOptsFile(argc, argv))
		ExitPostmaster(1);

	/*
	 * Set up signal handlers for the postmaster process.
	 */
	pqinitmask();
	PG_SETMASK(&BlockSig);

	pqsignal(SIGHUP, SIGHUP_handler);	/* reread config file and have children do same */
	pqsignal(SIGINT, pmdie);	/* send SIGTERM and ShutdownDataBase */
	pqsignal(SIGQUIT, pmdie);	/* send SIGUSR1 and die */
	pqsignal(SIGTERM, pmdie);	/* wait for children and ShutdownDataBase */
	pqsignal(SIGALRM, SIG_IGN); /* ignored */
	pqsignal(SIGPIPE, SIG_IGN); /* ignored */
	pqsignal(SIGUSR1, pmdie);	/* currently ignored, but see note in pmdie */
	pqsignal(SIGUSR2, pmdie);	/* send SIGUSR2, don't die */
	pqsignal(SIGCHLD, reaper);	/* handle child termination */
	pqsignal(SIGTTIN, SIG_IGN); /* ignored */
	pqsignal(SIGTTOU, SIG_IGN); /* ignored */
	pqsignal(SIGWINCH, dumpstatus);		/* dump port status */

	/*
	 * We're ready to rock and roll...
	 */
	StartupPID = StartupDataBase();

	status = ServerLoop();

	/*
	 * ServerLoop probably shouldn't ever return, but if it does, close down.
	 */
	ExitPostmaster(status != STATUS_OK);

	return 0;					/* not reached */
}

static void
pmdaemonize(int argc, char *argv[])
{
	int			i;
	pid_t		pid;

	pid = fork();
	if (pid == (pid_t) -1)
	{
		perror("Failed to fork postmaster");
		ExitPostmaster(1);
		return;					/* not reached */
	}
	else if (pid)
	{							/* parent */
		/* Parent should just exit, without doing any atexit cleanup */
		_exit(0);
	}

	MyProcPid = getpid();		/* reset MyProcPid to child */

/* GH: If there's no setsid(), we hopefully don't need silent mode.
 * Until there's a better solution.
 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		fprintf(stderr, "%s: ", progname);
		perror("cannot disassociate from controlling TTY");
		ExitPostmaster(1);
	}
#endif
	i = open(NULL_DEV, O_RDWR | PG_BINARY);
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);
	close(i);
}



/*
 * Print out help message
 */
static void
usage(const char *progname)
{
	printf("%s is the PostgreSQL server.\n\n", progname);
	printf("Usage:\n  %s [options...]\n\n", progname);
	printf("Options:\n");
#ifdef USE_ASSERT_CHECKING
	printf("  -A 1|0          enable/disable run-time assert checking\n");
#endif
	printf("  -B NBUFFERS     number of shared buffers (default %d)\n", DEF_NBUFFERS);
	printf("  -c NAME=VALUE   set run-time parameter\n");
	printf("  -d 1-5          debugging level\n");
	printf("  -D DATADIR      database directory\n");
	printf("  -F              turn fsync off\n");
	printf("  -h HOSTNAME     host name or IP address to listen on\n");
	printf("  -i              enable TCP/IP connections\n");
	printf("  -k DIRECTORY    Unix-domain socket location\n");
#ifdef USE_SSL
	printf("  -l              enable SSL connections\n");
#endif
	printf("  -N MAX-CONNECT  maximum number of allowed connections (1..%d, default %d)\n",
			MAXBACKENDS, DEF_MAXBACKENDS);
	printf("  -o OPTIONS      pass 'OPTIONS' to each backend server\n");
	printf("  -p PORT         port number to listen on (default %d)\n", DEF_PGPORT);
	printf("  -S              silent mode (start in background without logging output)\n");

	printf("\nDeveloper options:\n");
	printf("  -n              do not reinitialize shared memory after abnormal exit\n");
	printf("  -s              send SIGSTOP to all backend servers if one dies\n");

	printf("\nPlease read the documentation for the complete list of run-time\n"
		   "configuration settings and how to set them on the command line or in\n"
		   "the configuration file.\n\n");

	printf("Report bugs to <pgsql-bugs@postgresql.org>.\n");
}

static int
ServerLoop(void)
{
	fd_set		readmask,
				writemask;
	int			nSockets;
	Dlelem	   *curr;
	struct timeval now,
				later;
	struct timezone tz;

	gettimeofday(&now, &tz);

	nSockets = initMasks(&readmask, &writemask);

	for (;;)
	{
		Port		   *port;
		fd_set			rmask,
						wmask;
		struct timeval *timeout = NULL;
		struct timeval	timeout_tv;

		if (CheckPointPID == 0 && checkpointed && !FatalError)
		{
			time_t	now = time(NULL);

			if (CheckPointTimeout + checkpointed > now)
			{
				timeout_tv.tv_sec = CheckPointTimeout + checkpointed - now;
				timeout_tv.tv_usec = 0;
				timeout = &timeout_tv;
			}
			else
				CheckPointPID = CheckPointDataBase();
		}

#ifdef USE_SSL
		/*
		 * If we are using SSL, there may be input data already read and
		 * pending in SSL's input buffers.  If so, check for additional
		 * input from other clients, but don't delay before processing.
		 */
		for (curr = DLGetHead(PortList); curr; curr = DLGetSucc(curr))
		{
			Port	   *port = (Port *) DLE_VAL(curr);

			if (port->ssl && SSL_pending(port->ssl))
			{
				timeout_tv.tv_sec = 0;
				timeout_tv.tv_usec = 0;
				timeout = &timeout_tv;
				break;
			}
		}
#endif

		/*
		 * Wait for something to happen.
		 */
		memcpy((char *) &rmask, (char *) &readmask, sizeof(fd_set));
		memcpy((char *) &wmask, (char *) &writemask, sizeof(fd_set));

		PG_SETMASK(&UnBlockSig);

		if (select(nSockets, &rmask, &wmask, (fd_set *) NULL, timeout) < 0)
		{
			PG_SETMASK(&BlockSig);
			if (errno == EINTR || errno == EWOULDBLOCK)
				continue;
			fprintf(stderr, "%s: ServerLoop: select failed: %s\n",
					progname, strerror(errno));
			return STATUS_ERROR;
		}

		/*
		 * Block all signals until we wait again
		 */
		PG_SETMASK(&BlockSig);

		/*
		 * Respond to signals, if needed
		 */
		if (got_SIGHUP)
		{
			got_SIGHUP = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Select a random seed at the time of first receiving a request.
		 */
		while (random_seed == 0)
		{
			gettimeofday(&later, &tz);

			/*
			 * We are not sure how much precision is in tv_usec, so we
			 * swap the nibbles of 'later' and XOR them with 'now'. On the
			 * off chance that the result is 0, we loop until it isn't.
			 */
			random_seed = now.tv_usec ^
				((later.tv_usec << 16) |
				 ((later.tv_usec >> 16) & 0xffff));
		}

		/*
		 * new connection pending on our well-known port's socket?
		 */

#ifdef HAVE_UNIX_SOCKETS
		if (ServerSock_UNIX != INVALID_SOCK &&
			FD_ISSET(ServerSock_UNIX, &rmask) &&
			(port = ConnCreate(ServerSock_UNIX)) != NULL)
		{
			PacketReceiveSetup(&port->pktInfo,
							   readStartupPacket,
							   (void *) port);
		}
#endif

		if (ServerSock_INET != INVALID_SOCK &&
			FD_ISSET(ServerSock_INET, &rmask) &&
			(port = ConnCreate(ServerSock_INET)) != NULL)
		{
			PacketReceiveSetup(&port->pktInfo,
							   readStartupPacket,
							   (void *) port);
		}

		/*
		 * Scan active ports, processing any available input.  While we
		 * are at it, build up new masks for next select().
		 */
		nSockets = initMasks(&readmask, &writemask);

		curr = DLGetHead(PortList);

		while (curr)
		{
			Port	   *port = (Port *) DLE_VAL(curr);
			int			status = STATUS_OK;
			Dlelem	   *next;

			if (FD_ISSET(port->sock, &rmask)
#ifdef USE_SSL
				|| (port->ssl && SSL_pending(port->ssl))
#endif
				)
			{
				if (DebugLvl > 1)
					fprintf(stderr, "%s: ServerLoop:\t\thandling reading %d\n",
							progname, port->sock);

				if (PacketReceiveFragment(port) != STATUS_OK)
					status = STATUS_ERROR;
			}

			if (FD_ISSET(port->sock, &wmask))
			{
				if (DebugLvl > 1)
					fprintf(stderr, "%s: ServerLoop:\t\thandling writing %d\n",
							progname, port->sock);

				if (PacketSendFragment(port) != STATUS_OK)
					status = STATUS_ERROR;
			}

			/* Get this before the connection might be closed. */

			next = DLGetSucc(curr);

			/*
			 * If there is no error and no outstanding data transfer going
			 * on, then the authentication handshake must be complete to
			 * the postmaster's satisfaction.  So, start the backend.
			 */

			if (status == STATUS_OK && port->pktInfo.state == Idle)
			{
				/*
				 * Can we accept a connection now?
				 *
				 * Even though readStartupPacket() already checked,
				 * we have to check again in case conditions changed
				 * while negotiating authentication.
				 */
				char   *rejectMsg = canAcceptConnections();

				if (rejectMsg != NULL)
				{
					PacketSendError(&port->pktInfo, rejectMsg);
				}
				else
				{

					/*
					 * If the backend start fails then keep the connection
					 * open to report it.  Otherwise, pretend there is an
					 * error to close our descriptor for the connection,
					 * which will now be managed by the backend.
					 */
					if (BackendStartup(port) != STATUS_OK)
						PacketSendError(&port->pktInfo,
										"Backend startup failed");
					else
						status = STATUS_ERROR;
				}
			}

			/* Close the connection if required. */

			if (status != STATUS_OK)
			{
				StreamClose(port->sock);
				DLRemove(curr);
				ConnFree(port);
				DLFreeElem(curr);
			}
			else
			{
				/* Set the masks for this connection. */

				if (nSockets <= port->sock)
					nSockets = port->sock + 1;

				if (port->pktInfo.state == WritingPacket)
					FD_SET(port->sock, &writemask);
				else
					FD_SET(port->sock, &readmask);
			}

			curr = next;
		} /* loop over active ports */
	}
}


/*
 * Initialise the read and write masks for select() for the well-known ports
 * we are listening on.  Return the number of sockets to listen on.
 */

static int
initMasks(fd_set *rmask, fd_set *wmask)
{
	int			nsocks = -1;

	FD_ZERO(rmask);
	FD_ZERO(wmask);

#ifdef HAVE_UNIX_SOCKETS
	if (ServerSock_UNIX != INVALID_SOCK)
	{
		FD_SET(ServerSock_UNIX, rmask);

		if (ServerSock_UNIX > nsocks)
			nsocks = ServerSock_UNIX;
	}
#endif

	if (ServerSock_INET != INVALID_SOCK)
	{
		FD_SET(ServerSock_INET, rmask);
		if (ServerSock_INET > nsocks)
			nsocks = ServerSock_INET;
	}

	return nsocks + 1;
}


/*
 * Called when the startup packet has been read.
 */

static int
readStartupPacket(void *arg, PacketLen len, void *pkt)
{
	Port	   *port;
	StartupPacket *si;
	char	   *rejectMsg;

	port = (Port *) arg;
	si = (StartupPacket *) pkt;

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 */

	port->proto = ntohl(si->protoVersion);

	if (port->proto == CANCEL_REQUEST_CODE)
		return processCancelRequest(port, len, pkt);

	if (port->proto == NEGOTIATE_SSL_CODE)
	{
		char		SSLok;

#ifdef USE_SSL
		/* No SSL when disabled or on Unix sockets */
		if (!EnableSSL || port->laddr.sa.sa_family != AF_INET)
			SSLok = 'N';
		else
			SSLok = 'S';		/* Support for SSL */
#else
		SSLok = 'N';			/* No support for SSL */
#endif
		if (send(port->sock, &SSLok, 1, 0) != 1)
		{
			perror("Failed to send SSL negotiation response");
			return STATUS_ERROR; /* Close connection */
		}

#ifdef USE_SSL
		if (SSLok == 'S')
		{
		  if (!(port->ssl = SSL_new(SSL_context)) ||
		      !SSL_set_fd(port->ssl, port->sock) ||
		      SSL_accept(port->ssl) <= 0)
		    {
		      fprintf(stderr, "Failed to initialize SSL connection: %s, errno: %d (%s)\n",
			      ERR_reason_error_string(ERR_get_error()), errno, strerror(errno));
		      return STATUS_ERROR;
		    }
		}
#endif
		/* ready for the normal startup packet */
		PacketReceiveSetup(&port->pktInfo,
						   readStartupPacket,
						   (void *) port);
		return STATUS_OK;		/* Do not close connection */
	}

	/* Could add additional special packet types here */


	/* Check we can handle the protocol the frontend is using. */

	if (PG_PROTOCOL_MAJOR(port->proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(port->proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) ||
		(PG_PROTOCOL_MAJOR(port->proto) == PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) &&
		 PG_PROTOCOL_MINOR(port->proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST)))
	{
		PacketSendError(&port->pktInfo, "Unsupported frontend protocol.");
		return STATUS_OK;		/* don't close the connection yet */
	}

	/*
	 * Get the parameters from the startup packet as C strings.  The
	 * packet destination was cleared first so a short packet has zeros
	 * silently added and a long packet is silently truncated.
	 */

	StrNCpy(port->database, si->database, sizeof(port->database));
	StrNCpy(port->user, si->user, sizeof(port->user));
	StrNCpy(port->options, si->options, sizeof(port->options));
	StrNCpy(port->tty, si->tty, sizeof(port->tty));

	/* The database defaults to the user name. */

	if (port->database[0] == '\0')
		StrNCpy(port->database, si->user, sizeof(port->database));

	/* Check a user name was given. */

	if (port->user[0] == '\0')
	{
		PacketSendError(&port->pktInfo,
					"No Postgres username specified in startup packet.");
		return STATUS_OK;		/* don't close the connection yet */
	}

	/*
	 * If we're going to reject the connection due to database state,
	 * say so now instead of wasting cycles on an authentication exchange.
	 * (This also allows a pg_ping utility to be written.)
	 */
	rejectMsg = canAcceptConnections();

	if (rejectMsg != NULL)
	{
		PacketSendError(&port->pktInfo, rejectMsg);
		return STATUS_OK;		/* don't close the connection yet */
	}

	/* Start the authentication itself. */

	be_recvauth(port);

	return STATUS_OK;			/* don't close the connection yet */
}

/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-backend packet.	Perform the necessary processing.
 * Note that in any case, we return STATUS_ERROR to close the
 * connection immediately.	Nothing is sent back to the client.
 */

static int
processCancelRequest(Port *port, PacketLen len, void *pkt)
{
	CancelRequestPacket *canc = (CancelRequestPacket *) pkt;
	int			backendPID;
	long		cancelAuthCode;
	Dlelem	   *curr;
	Backend    *bp;

	backendPID = (int) ntohl(canc->backendPID);
	cancelAuthCode = (long) ntohl(canc->cancelAuthCode);

	if (backendPID == CheckPointPID)
	{
		if (DebugLvl)
			fprintf(stderr, "%s: processCancelRequest: CheckPointPID in cancel request for process %d\n",
					progname, backendPID);
		return STATUS_ERROR;
	}

	/* See if we have a matching backend */

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		bp = (Backend *) DLE_VAL(curr);
		if (bp->pid == backendPID)
		{
			if (bp->cancel_key == cancelAuthCode)
			{
				/* Found a match; signal that backend to cancel current op */
				if (DebugLvl)
					fprintf(stderr, "%s: processCancelRequest: sending SIGINT to process %d\n",
							progname, bp->pid);
				kill(bp->pid, SIGINT);
			}
			else
			{
				/* Right PID, wrong key: no way, Jose */
				if (DebugLvl)
					fprintf(stderr, "%s: processCancelRequest: bad key in cancel request for process %d\n",
							progname, bp->pid);
			}
			return STATUS_ERROR;
		}
	}

	/* No matching backend */
	if (DebugLvl)
		fprintf(stderr, "%s: processCancelRequest: bad PID in cancel request for process %d\n",
				progname, backendPID);

	return STATUS_ERROR;
}

/*
 * canAcceptConnections --- check to see if database state allows connections.
 *
 * If we are open for business, return NULL, otherwise return an error message
 * string suitable for rejecting a connection request.
 */
static char *
canAcceptConnections(void)
{
	/* Can't start backends when in startup/shutdown/recovery state. */
	if (Shutdown > NoShutdown)
		return "The Data Base System is shutting down";
	if (StartupPID)
		return "The Data Base System is starting up";
	if (FatalError)
		return "The Data Base System is in recovery mode";
	/* Can't start backend if max backend count is exceeded. */
	if (CountChildren() >= MaxBackends)
		return "Sorry, too many clients already";

	return NULL;
}

/*
 * ConnCreate -- create a local connection data structure
 */
static Port *
ConnCreate(int serverFd)
{
	Port	   *port;

	if (!(port = (Port *) calloc(1, sizeof(Port))))
	{
		fprintf(stderr, "%s: ConnCreate: malloc failed\n",
				progname);
		SignalChildren(SIGUSR1);
		ExitPostmaster(1);
	}

	if (StreamConnection(serverFd, port) != STATUS_OK)
	{
		StreamClose(port->sock);
		ConnFree(port);
		port = NULL;
	}
	else
	{
		DLAddHead(PortList, DLNewElem(port));
		RandomSalt(port->salt);
		port->pktInfo.state = Idle;
	}

	return port;
}

/*
 * ConnFree -- free a local connection data structure
 */
static void
ConnFree(Port *conn)
{
#ifdef USE_SSL
	if (conn->ssl)
		SSL_free(conn->ssl);
#endif
	free(conn);
}


/*
 * reset_shared -- reset shared memory and semaphores
 */
static void
reset_shared(unsigned short port)
{
	/*
	 * Reset assignment of shared mem and semaphore IPC keys.
	 * Doing this means that in normal cases we'll assign the same keys
	 * on each "cycle of life", and thereby avoid leaving dead IPC objects
	 * floating around if the postmaster crashes and is restarted.
	 */
	IpcInitKeyAssignment(port);
	/*
	 * Create or re-create shared memory and semaphores.
	 */
	CreateSharedMemoryAndSemaphores(false, MaxBackends);
}


/*
 * Set flag if SIGHUP was detected so config file can be reread in
 * main loop
 */
static void
SIGHUP_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	if (Shutdown > SmartShutdown)
		return;
	got_SIGHUP = true;
	SignalChildren(SIGHUP);
	errno = save_errno;
}



/*
 * pmdie -- signal handler for cleaning up after a kill signal.
 */
static void
pmdie(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	if (DebugLvl >= 1)
		elog(DEBUG, "pmdie %d", postgres_signal_arg);

	switch (postgres_signal_arg)
	{
		case SIGUSR1:
			/*
			 * Currently the postmaster ignores SIGUSR1 (maybe it should
			 * do something useful instead?)  But we must have some handler
			 * installed for SIGUSR1, not just set it to SIG_IGN.  Else, a
			 * freshly spawned backend would likewise have it set to SIG_IGN,
			 * which would mean the backend would ignore any attempt to kill
			 * it before it had gotten as far as setting up its own handler.
			 */
			errno = save_errno;
			return;

		case SIGUSR2:

			/*
			 * Send SIGUSR2 to all children (AsyncNotifyHandler)
			 */
			if (Shutdown > SmartShutdown)
			{
				errno = save_errno;
				return;
			}
			SignalChildren(SIGUSR2);
			errno = save_errno;
			return;

		case SIGTERM:

			/*
			 * Smart Shutdown:
			 *
			 * let children to end their work and ShutdownDataBase.
			 */
			if (Shutdown >= SmartShutdown)
			{
				errno = save_errno;
				return;
			}
			Shutdown = SmartShutdown;
			tnow = time(NULL);
			fprintf(stderr, "Smart Shutdown request at %s", ctime(&tnow));
			fflush(stderr);
			if (DLGetHead(BackendList)) /* let reaper() handle this */
			{
				errno = save_errno;
				return;
			}

			/*
			 * No children left. Shutdown data base system.
			 */
			if (StartupPID > 0 || FatalError)	/* let reaper() handle
												 * this */
			{
				errno = save_errno;
				return;
			}
			if (ShutdownPID > 0)
				abort();

			ShutdownPID = ShutdownDataBase();
			errno = save_errno;
			return;

		case SIGINT:

			/*
			 * Fast Shutdown:
			 *
			 * abort all children with SIGTERM (rollback active transactions
			 * and exit) and ShutdownDataBase.
			 */
			if (Shutdown >= FastShutdown)
			{
				errno = save_errno;
				return;
			}
			tnow = time(NULL);
			fprintf(stderr, "Fast Shutdown request at %s", ctime(&tnow));
			fflush(stderr);
			if (DLGetHead(BackendList)) /* let reaper() handle this */
			{
				Shutdown = FastShutdown;
				if (!FatalError)
				{
					fprintf(stderr, "Aborting any active transaction...\n");
					fflush(stderr);
					SignalChildren(SIGTERM);
				}
				errno = save_errno;
				return;
			}
			if (Shutdown > NoShutdown)
			{
				Shutdown = FastShutdown;
				errno = save_errno;
				return;
			}
			Shutdown = FastShutdown;

			/*
			 * No children left. Shutdown data base system.
			 */
			if (StartupPID > 0 || FatalError)	/* let reaper() handle
												 * this */
			{
				errno = save_errno;
				return;
			}
			if (ShutdownPID > 0)
				abort();

			ShutdownPID = ShutdownDataBase();
			errno = save_errno;
			return;

		case SIGQUIT:

			/*
			 * Immediate Shutdown:
			 *
			 * abort all children with SIGUSR1 and exit without attempt to
			 * properly shutdown data base system.
			 */
			tnow = time(NULL);
			fprintf(stderr, "Immediate Shutdown request at %s", ctime(&tnow));
			fflush(stderr);
			if (ShutdownPID > 0)
				kill(ShutdownPID, SIGQUIT);
			else if (StartupPID > 0)
				kill(StartupPID, SIGQUIT);
			else if (DLGetHead(BackendList))
				SignalChildren(SIGUSR1);
			break;
	}

	/* exit postmaster */
	ExitPostmaster(0);
}

/*
 * Reaper -- signal handler to cleanup after a backend (child) dies.
 */
static void
reaper(SIGNAL_ARGS)
{
	int			save_errno = errno;
#ifdef HAVE_WAITPID
	int			status;			/* backend exit status */
#else
	union wait	status;			/* backend exit status */
#endif
	int			exitstatus;
	int			pid;			/* process id of dead backend */

	PG_SETMASK(&BlockSig);
	/* It's not really necessary to reset the handler each time is it? */
	pqsignal(SIGCHLD, reaper);

	if (DebugLvl)
		fprintf(stderr, "%s: reaping dead processes...\n",
				progname);
#ifdef HAVE_WAITPID
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		exitstatus = status;
#else
	while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
	{
		exitstatus = status.w_status;
#endif
		if (ShutdownPID > 0)
		{
			if (pid != ShutdownPID)
				abort();
			if (exitstatus != 0)
			{
				fprintf(stderr, "Shutdown failed - abort\n");
				fflush(stderr);
				ExitPostmaster(1);
			}
			ExitPostmaster(0);
		}
		if (StartupPID > 0)
		{
			if (pid != StartupPID)
				abort();
			if (exitstatus != 0)
			{
				fprintf(stderr, "Startup failed - abort\n");
				fflush(stderr);
				ExitPostmaster(1);
			}
			StartupPID = 0;
			FatalError = false;	/* done with recovery */
			if (Shutdown > NoShutdown)
			{
				if (ShutdownPID > 0)
					abort();
				ShutdownPID = ShutdownDataBase();
			}

			/*
			 * Startup succeeded - remember its ID
			 * and RedoRecPtr
			 */
			SetThisStartUpID();

			CheckPointPID = 0;
			checkpointed = time(NULL);

			errno = save_errno;
			return;
		}
		CleanupProc(pid, exitstatus);
	}

	if (FatalError)
	{

		/*
		 * Wait for all children exit, then reset shmem and StartupDataBase.
		 */
		if (DLGetHead(BackendList) || StartupPID > 0 || ShutdownPID > 0)
		{
			errno = save_errno;
			return;
		}
		tnow = time(NULL);
		fprintf(stderr, "Server processes were terminated at %s"
				"Reinitializing shared memory and semaphores\n",
				ctime(&tnow));
		fflush(stderr);

		shmem_exit(0);
		reset_shared(PostPortNumber);

		StartupPID = StartupDataBase();
		errno = save_errno;
		return;
	}

	if (Shutdown > NoShutdown)
	{
		if (DLGetHead(BackendList))
		{
			errno = save_errno;
			return;
		}
		if (StartupPID > 0 || ShutdownPID > 0)
		{
			errno = save_errno;
			return;
		}
		ShutdownPID = ShutdownDataBase();
	}

	errno = save_errno;
}

/*
 * CleanupProc -- cleanup after terminated backend.
 *
 * Remove all local state associated with backend.
 *
 * Dillon's note: should log child's exit status in the system log.
 */
static void
CleanupProc(int pid,
			int exitstatus)		/* child's exit status. */
{
	Dlelem	   *curr,
			   *next;
	Backend    *bp;

	if (DebugLvl)
		fprintf(stderr, "%s: CleanupProc: pid %d exited with status %d\n",
				progname, pid, exitstatus);

	/*
	 * If a backend dies in an ugly way (i.e. exit status not 0) then we
	 * must signal all other backends to quickdie.	If exit status is zero
	 * we assume everything is hunky dory and simply remove the backend
	 * from the active backend list.
	 */
	if (exitstatus == 0)
	{
		curr = DLGetHead(BackendList);
		while (curr)
		{
			bp = (Backend *) DLE_VAL(curr);
			if (bp->pid == pid)
			{
				DLRemove(curr);
				free(bp);
				DLFreeElem(curr);
				break;
			}
			curr = DLGetSucc(curr);
		}

		if (pid == CheckPointPID)
		{
			CheckPointPID = 0;
			if (!FatalError)
			{
				checkpointed = time(NULL);
				GetRedoRecPtr();
			}
		}
		else
		{
			/* Why is this done here, and not by the backend itself? */
			if (!FatalError)
				ProcRemove(pid);
		}

		return;
	}

	if (!FatalError)
	{
		/* Make log entry unless we did so already */
		tnow = time(NULL);
		fprintf(stderr, "Server process (pid %d) exited with status %d at %s"
				"Terminating any active server processes...\n",
				pid, exitstatus, ctime(&tnow));
		fflush(stderr);
	}

	curr = DLGetHead(BackendList);
	while (curr)
	{
		next = DLGetSucc(curr);
		bp = (Backend *) DLE_VAL(curr);
		if (bp->pid != pid)
		{
			/*
			 * This backend is still alive.  Unless we did so already,
			 * tell it to commit hara-kiri.
			 *
			 * SIGUSR1 is the special signal that says exit without proc_exit
			 * and let the user know what's going on. But if SendStop is set
			 * (-s on command line), then we send SIGSTOP instead, so that we
			 * can get core dumps from all backends by hand.
			 */
			if (!FatalError)
			{
				if (DebugLvl)
					fprintf(stderr, "%s: CleanupProc: sending %s to process %d\n",
							progname,
							(SendStop ? "SIGSTOP" : "SIGUSR1"),
							bp->pid);
				kill(bp->pid, (SendStop ? SIGSTOP : SIGUSR1));
			}
		}
		else
		{
			/*
			 * Found entry for freshly-dead backend, so remove it.
			 *
			 * Don't call ProcRemove() here, since shmem may be corrupted!
			 * We are going to reinitialize shmem and semaphores anyway
			 * once all the children are dead, so no need for it.
			 */
			DLRemove(curr);
			free(bp);
			DLFreeElem(curr);
		}
		curr = next;
	}

	if (pid == CheckPointPID)
	{
		CheckPointPID = 0;
		checkpointed = 0;
	}

	FatalError = true;
}

/*
 * Send a signal to all chidren processes.
 */
static void
SignalChildren(int signal)
{
	Dlelem	   *curr,
			   *next;
	Backend    *bp;
	int			mypid = getpid();

	curr = DLGetHead(BackendList);
	while (curr)
	{
		next = DLGetSucc(curr);
		bp = (Backend *) DLE_VAL(curr);

		if (bp->pid != mypid)
		{
			if (DebugLvl >= 1)
				elog(DEBUG, "SignalChildren: sending signal %d to process %d",
					 signal, bp->pid);

			kill(bp->pid, signal);
		}

		curr = next;
	}
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork/exec failed, STATUS_OK
 *		otherwise.
 *
 */
static int
BackendStartup(Port *port)
{
	Backend    *bn;				/* for backend cleanup */
	int			pid;

	/*
	 * Compute the cancel key that will be assigned to this backend. The
	 * backend will have its own copy in the forked-off process' value of
	 * MyCancelKey, so that it can transmit the key to the frontend.
	 */
	MyCancelKey = PostmasterRandom();

	/*
	 * Flush stdio channels just before fork, to avoid double-output
	 * problems. Ideally we'd use fflush(NULL) here, but there are still a
	 * few non-ANSI stdio libraries out there (like SunOS 4.1.x) that
	 * coredump if we do. Presently stdout and stderr are the only stdio
	 * output channels used by the postmaster, so fflush'ing them should
	 * be sufficient.
	 */
	fflush(stdout);
	fflush(stderr);

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

	if ((pid = fork()) == 0)
	{							/* child */
#ifdef __BEOS__
		/* Specific beos backend startup actions */
		beos_backend_startup();
#endif

#ifdef CYR_RECODE
		{
			/* Save charset for this host while we still have client addr */
			char		ChTable[80];
			static char cyrEnvironment[100];

			GetCharSetByHost(ChTable, port->raddr.in.sin_addr.s_addr, DataDir);
			if (*ChTable != '\0')
			{
				snprintf(cyrEnvironment, sizeof(cyrEnvironment),
						 "PG_RECODETABLE=%s", ChTable);
				putenv(cyrEnvironment);
			}
		}
#endif

		if (DoBackend(port))
		{
			fprintf(stderr, "%s child[%d]: BackendStartup: backend startup failed\n",
					progname, (int) getpid());
			ExitPostmaster(1);
		}
		else
			ExitPostmaster(0);
	}

	/* in parent */
	if (pid < 0)
	{
#ifdef __BEOS__
		/* Specific beos backend startup actions */
		beos_backend_startup_failed();
#endif
		fprintf(stderr, "%s: BackendStartup: fork failed: %s\n",
				progname, strerror(errno));
		return STATUS_ERROR;
	}

	if (DebugLvl)
		fprintf(stderr, "%s: BackendStartup: pid %d user %s db %s socket %d\n",
				progname, pid, port->user, port->database,
				port->sock);

	/*
	 * Everything's been successful, it's safe to add this backend to our
	 * list of backends.
	 */
	if (!(bn = (Backend *) calloc(1, sizeof(Backend))))
	{
		fprintf(stderr, "%s: BackendStartup: malloc failed\n",
				progname);
		ExitPostmaster(1);
	}

	bn->pid = pid;
	bn->cancel_key = MyCancelKey;
	DLAddHead(BackendList, DLNewElem(bn));

	return STATUS_OK;
}

/*
 * split_opts -- split a string of options and append it to an argv array
 *
 * NB: the string is destructively modified!
 *
 * Since no current POSTGRES arguments require any quoting characters,
 * we can use the simple-minded tactic of assuming each set of space-
 * delimited characters is a separate argv element.
 *
 * If you don't like that, well, we *used* to pass the whole option string
 * as ONE argument to execl(), which was even less intelligent...
 */
static void
split_opts(char **argv, int *argcp, char *s)
{
	while (s && *s)
	{
		while (isspace((unsigned char) *s))
			++s;
		if (*s == '\0')
			break;
		argv[(*argcp)++] = s;
		while (*s && !isspace((unsigned char) *s))
			++s;
		if (*s)
			*s++ = '\0';
	}
}

/*
 * DoBackend -- set up the backend's argument list and invoke backend main().
 *
 * This used to perform an execv() but we no longer exec the backend;
 * it's the same executable as the postmaster.
 *
 * returns:
 *		Shouldn't return at all.
 *		If PostgresMain() fails, return status.
 */
static int
DoBackend(Port *port)
{
	char	   *av[ARGV_SIZE * 2];
	int			ac = 0;
	char		debugbuf[ARGV_SIZE];
	char		protobuf[ARGV_SIZE];
	char		dbbuf[ARGV_SIZE];
	char		optbuf[ARGV_SIZE];
	char		ttybuf[ARGV_SIZE];
	int			i;
	struct timeval now;
	struct timezone tz;

	/*
	 * Let's clean up ourselves as the postmaster child
	 */

	/* We don't want the postmaster's proc_exit() handlers */
	on_exit_reset();

	/*
	 * Signal handlers setting is moved to tcop/postgres...
	 */

	/* Close the postmaster sockets */
	if (NetServer)
		StreamClose(ServerSock_INET);
	ServerSock_INET = INVALID_SOCK;
#ifdef HAVE_UNIX_SOCKETS
	StreamClose(ServerSock_UNIX);
	ServerSock_UNIX = INVALID_SOCK;
#endif

	/* Save port etc. for ps status */
	MyProcPort = port;

	/* Reset MyProcPid to new backend's pid */
	MyProcPid = getpid();

	/*
	 * Don't want backend to be able to see the postmaster random number
	 * generator state.  We have to clobber the static random_seed *and*
	 * start a new random sequence in the random() library function.
	 */
	random_seed = 0;
	gettimeofday(&now, &tz);
	srandom((unsigned int) now.tv_usec);

	/* ----------------
	 * Now, build the argv vector that will be given to PostgresMain.
	 *
	 * The layout of the command line is
	 *		postgres [secure switches] -p databasename [insecure switches]
	 * where the switches after -p come from the client request.
	 * ----------------
	 */

	av[ac++] = "postgres";

	/*
	 * Pass the requested debugging level along to the backend. Level one
	 * debugging in the postmaster traces postmaster connection activity,
	 * and levels two and higher are passed along to the backend.  This
	 * allows us to watch only the postmaster or the postmaster and the
	 * backend.
	 */
	if (DebugLvl > 1)
	{
		sprintf(debugbuf, "-d%d", DebugLvl);
		av[ac++] = debugbuf;
	}

	/*
	 * Pass any backend switches specified with -o in the postmaster's own
	 * command line.  We assume these are secure. (It's OK to mangle
	 * ExtraOptions since we are now in the child process; this won't
	 * change the postmaster's copy.)
	 */
	split_opts(av, &ac, ExtraOptions);

	/* Tell the backend what protocol the frontend is using. */
	sprintf(protobuf, "-v%u", port->proto);
	av[ac++] = protobuf;

	/*
	 * Tell the backend it is being called from the postmaster, and which
	 * database to use.  -p marks the end of secure switches.
	 */
	av[ac++] = "-p";

	StrNCpy(dbbuf, port->database, ARGV_SIZE);
	av[ac++] = dbbuf;

	/*
	 * Pass the (insecure) option switches from the connection request.
	 */
	StrNCpy(optbuf, port->options, ARGV_SIZE);
	split_opts(av, &ac, optbuf);

	/*
	 * Pass the (insecure) debug output file request.
	 *
	 * NOTE: currently, this is useless code, since the backend will not
	 * honor an insecure -o switch.  I left it here since the backend
	 * could be modified to allow insecure -o, given adequate checking
	 * that the specified filename is something safe to write on.
	 */
	if (port->tty[0])
	{
		StrNCpy(ttybuf, port->tty, ARGV_SIZE);
		av[ac++] = "-o";
		av[ac++] = ttybuf;
	}

	av[ac] = (char *) NULL;

	/*
	 * Release postmaster's working memory context so that backend can
	 * recycle the space.  Note this does not trash *MyProcPort, because
	 * ConnCreate() allocated that space with malloc() ... else we'd need
	 * to copy the Port data here.
	 */
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(PostmasterContext);
	PostmasterContext = NULL;

	/*
	 * Debug: print arguments being passed to backend
	 */
	if (DebugLvl > 1)
	{
		fprintf(stderr, "%s child[%d]: starting with (",
				progname, MyProcPid);
		for (i = 0; i < ac; ++i)
			fprintf(stderr, "%s ", av[i]);
		fprintf(stderr, ")\n");
	}

	return (PostgresMain(ac, av, real_argc, real_argv, port->user));
}

/*
 * ExitPostmaster -- cleanup
 *
 * Do NOT call exit() directly --- always go through here!
 */
static void
ExitPostmaster(int status)
{
	/* should cleanup shared memory and kill all backends */

	/*
	 * Not sure of the semantics here.	When the Postmaster dies, should
	 * the backends all be killed? probably not.
	 *
	 * MUST		-- vadim 05-10-1999
	 */
	if (ServerSock_INET != INVALID_SOCK)
		StreamClose(ServerSock_INET);
	ServerSock_INET = INVALID_SOCK;
#ifdef HAVE_UNIX_SOCKETS
	if (ServerSock_UNIX != INVALID_SOCK)
		StreamClose(ServerSock_UNIX);
	ServerSock_UNIX = INVALID_SOCK;
#endif

	proc_exit(status);
}

static void
dumpstatus(SIGNAL_ARGS)
{
	int			save_errno = errno;
	Dlelem	   *curr;

	PG_SETMASK(&BlockSig);

	curr = DLGetHead(PortList);
	while (curr)
	{
		Port	   *port = DLE_VAL(curr);

		fprintf(stderr, "%s: dumpstatus:\n", progname);
		fprintf(stderr, "\tsock %d\n", port->sock);
		curr = DLGetSucc(curr);
	}
	errno = save_errno;
}

/*
 * CharRemap
 */
static char
CharRemap(long int ch)
{

	if (ch < 0)
		ch = -ch;

	ch = ch % 62;
	if (ch < 26)
		return 'A' + ch;

	ch -= 26;
	if (ch < 26)
		return 'a' + ch;

	ch -= 26;
	return '0' + ch;
}

/*
 * RandomSalt
 */
static void
RandomSalt(char *salt)
{
	long		rand = PostmasterRandom();

	*salt = CharRemap(rand % 62);
	*(salt + 1) = CharRemap(rand / 62);
}

/*
 * PostmasterRandom
 */
static long
PostmasterRandom(void)
{
	static bool initialized = false;

	if (!initialized)
	{
		Assert(random_seed != 0);
		srandom(random_seed);
		initialized = true;
	}

	return random() ^ random_seed;
}

/*
 * Count up number of child processes.
 */
static int
CountChildren(void)
{
	Dlelem	   *curr;
	Backend    *bp;
	int			mypid = getpid();
	int			cnt = 0;

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		bp = (Backend *) DLE_VAL(curr);
		if (bp->pid != mypid)
			cnt++;
	}
	if (CheckPointPID != 0)
		cnt--;
	return cnt;
}

#ifdef USE_SSL
/*
 * Initialize SSL library and structures
 */
static void
InitSSL(void)
{
	char		fnbuf[2048];

	SSL_load_error_strings();
	SSL_library_init();
	SSL_context = SSL_CTX_new(SSLv23_method());
	if (!SSL_context)
	{
		fprintf(stderr, "Failed to create SSL context: %s\n", ERR_reason_error_string(ERR_get_error()));
		ExitPostmaster(1);
	}
	snprintf(fnbuf, sizeof(fnbuf), "%s/server.crt", DataDir);
	if (!SSL_CTX_use_certificate_file(SSL_context, fnbuf, SSL_FILETYPE_PEM))
	{
		fprintf(stderr, "Failed to load server certificate (%s): %s\n", fnbuf, ERR_reason_error_string(ERR_get_error()));
		ExitPostmaster(1);
	}
	snprintf(fnbuf, sizeof(fnbuf), "%s/server.key", DataDir);
	if (!SSL_CTX_use_PrivateKey_file(SSL_context, fnbuf, SSL_FILETYPE_PEM))
	{
		fprintf(stderr, "Failed to load private key file (%s): %s\n", fnbuf, ERR_reason_error_string(ERR_get_error()));
		ExitPostmaster(1);
	}
	if (!SSL_CTX_check_private_key(SSL_context))
	{
		fprintf(stderr, "Check of private key failed: %s\n", ERR_reason_error_string(ERR_get_error()));
		ExitPostmaster(1);
	}
}

#endif

static pid_t
SSDataBase(int xlop)
{
	pid_t		pid;
	Backend	   *bn;

	fflush(stdout);
	fflush(stderr);

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

	if ((pid = fork()) == 0)	/* child */
	{
		char	   *av[ARGV_SIZE * 2];
		int			ac = 0;
		char		nbbuf[ARGV_SIZE];
		char		dbbuf[ARGV_SIZE];
		char		xlbuf[ARGV_SIZE];

#ifdef __BEOS__
		/* Specific beos actions after backend startup */
		beos_backend_startup();
#endif

		/* Lose the postmaster's on-exit routines and port connections */
		on_exit_reset();

		if (NetServer)
			StreamClose(ServerSock_INET);
		ServerSock_INET = INVALID_SOCK;
#ifdef HAVE_UNIX_SOCKETS
		StreamClose(ServerSock_UNIX);
		ServerSock_UNIX = INVALID_SOCK;
#endif

		av[ac++] = "postgres";

		av[ac++] = "-d";

		sprintf(nbbuf, "-B%u", NBuffers);
		av[ac++] = nbbuf;

		sprintf(xlbuf, "-x %d", xlop);
		av[ac++] = xlbuf;

		av[ac++] = "-p";

		StrNCpy(dbbuf, "template1", ARGV_SIZE);
		av[ac++] = dbbuf;

		av[ac] = (char *) NULL;

		optind = 1;

		pqsignal(SIGQUIT, SIG_DFL);
#ifdef HAVE_SIGPROCMASK
		sigdelset(&BlockSig, SIGQUIT);
#else
		BlockSig &= ~(sigmask(SIGQUIT));
#endif
		PG_SETMASK(&BlockSig);

		BootstrapMain(ac, av);
		ExitPostmaster(0);
	}

	/* in parent */
	if (pid < 0)
	{
#ifdef __BEOS__
		/* Specific beos actions before backend startup */
		beos_backend_startup_failed();
#endif

		fprintf(stderr, "%s Data Base: fork failed: %s\n",
				((xlop == BS_XLOG_STARTUP) ? "Startup" : 
					((xlop == BS_XLOG_CHECKPOINT) ? "CheckPoint" :
						"Shutdown")), strerror(errno));
		ExitPostmaster(1);
	}

	if (xlop != BS_XLOG_CHECKPOINT)
		return(pid);

	if (!(bn = (Backend *) calloc(1, sizeof(Backend))))
	{
		fprintf(stderr, "%s: CheckPointDataBase: malloc failed\n",
				progname);
		ExitPostmaster(1);
	}

	bn->pid = pid;
	bn->cancel_key = 0;
	DLAddHead(BackendList, DLNewElem(bn));

	return (pid);
}


/*
 * Create the opts file
 */
static bool
CreateOptsFile(int argc, char *argv[])
{
	char    fullprogname[MAXPGPATH];
	char   *filename;
	FILE   *fp;
	unsigned i;

	if (FindExec(fullprogname, argv[0], "postmaster") == -1)
		return false;

	filename = palloc(strlen(DataDir) + 20);
	sprintf(filename, "%s/postmaster.opts", DataDir);

	fp = fopen(filename, "w");
	if (fp == NULL)
	{
		fprintf(stderr, "%s: cannot create file %s: %s\n", progname,
				filename, strerror(errno));
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " '%s'", argv[i]);
	fputs("\n", fp);

	if (ferror(fp))
	{
		fprintf(stderr, "%s: writing file %s failed\n", progname, filename);
		fclose(fp);
		return false;
	}

	fclose(fp);
	return true;
}
