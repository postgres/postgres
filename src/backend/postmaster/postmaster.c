/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.	Frontend programs send a startup message
 *	  to the Postmaster and the postmaster uses the info in the
 *	  message to setup a backend process.
 *
 *	  The postmaster also manages system-wide operations such as
 *	  startup, shutdown, and periodic checkpoints.	The postmaster
 *	  itself doesn't do those operations, mind you --- it just forks
 *	  off a subprocess to do them at the right times.  It also takes
 *	  care of resetting the system if a backend crashes.
 *
 *	  The postmaster process creates the shared memory and semaphore
 *	  pools during startup, but as a rule does not touch them itself.
 *	  In particular, it is not a member of the PGPROC array of backends
 *	  and so it cannot participate in lock-manager operations.	Keeping
 *	  the postmaster away from shared memory operations makes it simpler
 *	  and more reliable.  The postmaster is almost always able to recover
 *	  from crashes of individual backends by resetting shared memory;
 *	  if it did much with shared memory then it would be prone to crashing
 *	  along with the backends.
 *
 *	  When a request message is received, we now fork() immediately.
 *	  The child process performs authentication of the request, and
 *	  then becomes a backend if successful.  This allows the auth code
 *	  to be written in a simple single-threaded style (as opposed to the
 *	  crufty "poor man's multitasking" code that used to be needed).
 *	  More importantly, it ensures that blockages in non-multithreaded
 *	  libraries like SSL or PAM cannot cause denial of service to other
 *	  clients.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/postmaster/postmaster.c,v 1.347.2.3 2006/03/18 22:10:44 neilc Exp $
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up a few shared memory data structures
 *		for the backends.  It should at the very least initialize the
 *		lock manager.
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends but should avoid
 *		touching shared memory, so as not to become stuck if a crashing
 *		backend screws up locks or shared memory.  Likewise, the Postmaster
 *		should never block on messages from frontend clients.
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an emergency
 *		exit and/or core dump.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <limits.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifdef USE_RENDEZVOUS
#include <DNSServiceDiscovery/DNSServiceDiscovery.h>
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
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "access/xlog.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "bootstrap/bootstrap.h"
#include "pgstat.h"


#define INVALID_SOCK	(-1)

#ifdef HAVE_SIGPROCMASK
sigset_t	UnBlockSig,
			BlockSig,
			AuthBlockSig;

#else
int			UnBlockSig,
			BlockSig,
			AuthBlockSig;
#endif

/*
 * List of active backends (or child processes anyway; we don't actually
 * know whether a given child has become a backend or is still in the
 * authorization phase).  This is used mainly to keep track of how many
 * children we have and send them appropriate signals when necessary.
 */
typedef struct bkend
{
	pid_t		pid;			/* process id of backend */
	long		cancel_key;		/* cancel key for cancels for this backend */
} Backend;

static Dllist *BackendList;

/* The socket number we are listening for connections on */
int			PostPortNumber;
char	   *UnixSocketDir;
char	   *VirtualHost;

/*
 * MaxBackends is the limit on the number of backends we can start.
 * Note that a larger MaxBackends value will increase the size of the
 * shared memory area as well as cause the postmaster to grab more
 * kernel semaphores, even if you never actually use that many
 * backends.
 */
int			MaxBackends;

/*
 * ReservedBackends is the number of backends reserved for superuser use.
 * This number is taken out of the pool size given by MaxBackends so
 * number of backend slots available to non-superusers is
 * (MaxBackends - ReservedBackends).  Note what this really means is
 * "if there are <= ReservedBackends connections available, only superusers
 * can make new connections" --- pre-existing superuser connections don't
 * count against the limit.
 */
int			ReservedBackends;


static char *progname = (char *) NULL;

/* The socket(s) we're listening to. */
#define MAXLISTEN	10
static int	ListenSocket[MAXLISTEN];

/* Used to reduce macros tests */
#ifdef EXEC_BACKEND
const bool	ExecBackend = true;

#else
const bool	ExecBackend = false;
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

/* still more option variables */
bool		NetServer = false;	/* listen on TCP/IP */
bool		EnableSSL = false;
bool		SilentMode = false; /* silent mode (-S) */

int			PreAuthDelay = 0;
int			AuthenticationTimeout = 60;
int			CheckPointTimeout = 300;
int			CheckPointWarning = 30;
time_t		LastSignalledCheckpoint = 0;

bool		log_hostname;		/* for ps display */
bool		LogSourcePort;
bool		Log_connections = false;
bool		Db_user_namespace = false;

char	   *rendezvous_name;

/* For FNCTL_NONBLOCK */
#if defined(WIN32) || defined(__BEOS__)
long		ioctlsocket_ret = 1;
#endif

/* list of library:init-function to be preloaded */
char	   *preload_libraries_string = NULL;

/* Startup/shutdown state */
static pid_t StartupPID = 0,
			ShutdownPID = 0,
			CheckPointPID = 0;
static time_t checkpointed = 0;

#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* T if recovering from backend crash */

bool		ClientAuthInProgress = false;		/* T during new-client
												 * authentication */

/*
 * State for assigning random salts and cancel keys.
 * Also, the global MyCancelKey passes the cancel key assigned to a given
 * backend from the postmaster to that backend (via fork).
 */

static unsigned int random_seed = 0;

static int	debug_flag = 0;

extern char *optarg;
extern int	optind,
			opterr;

#ifdef HAVE_INT_OPTRESET
extern int	optreset;
#endif

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
static void sigusr1_handler(SIGNAL_ARGS);
static void dummy_handler(SIGNAL_ARGS);
static void CleanupProc(int pid, int exitstatus);
static void LogChildExit(int lev, const char *procname,
			 int pid, int exitstatus);
static int	BackendFork(Port *port);
static void ExitPostmaster(int status);
static void usage(const char *);
static int	ServerLoop(void);
static int	BackendStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool SSLdone);
static void processCancelRequest(Port *port, void *pkt);
static int	initMasks(fd_set *rmask);
static void report_fork_failure_to_client(Port *port, int errnum);
enum CAC_state
{
	CAC_OK, CAC_STARTUP, CAC_SHUTDOWN, CAC_RECOVERY, CAC_TOOMANY
};
static enum CAC_state canAcceptConnections(void);
static long PostmasterRandom(void);
static void RandomSalt(char *cryptSalt, char *md5Salt);
static void SignalChildren(int signal);
static int	CountChildren(void);
static bool CreateOptsFile(int argc, char *argv[]);
static pid_t SSDataBase(int xlop);
static void
postmaster_error(const char *fmt,...)
/* This lets gcc check the format string for consistency. */
__attribute__((format(printf, 1, 2)));

#define StartupDataBase()		SSDataBase(BS_XLOG_STARTUP)
#define CheckPointDataBase()	SSDataBase(BS_XLOG_CHECKPOINT)
#define ShutdownDataBase()		SSDataBase(BS_XLOG_SHUTDOWN)


static void
checkDataDir(const char *checkdir)
{
	char		path[MAXPGPATH];
	FILE	   *fp;
	struct stat stat_buf;

	if (checkdir == NULL)
	{
		fprintf(stderr,
				gettext("%s does not know where to find the database system data.\n"
						"You must specify the directory that contains the database system\n"
						"either by specifying the -D invocation option or by setting the\n"
						"PGDATA environment variable.\n"),
				progname);
		ExitPostmaster(2);
	}

	if (stat(checkdir, &stat_buf) == -1)
	{
		if (errno == ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("data directory \"%s\" does not exist",
							checkdir)));
		else
			ereport(FATAL,
					(errcode_for_file_access(),
			 errmsg("could not read permissions of directory \"%s\": %m",
					checkdir)));
	}

	/*
	 * Check if the directory has group or world access.  If so, reject.
	 *
	 * XXX temporarily suppress check when on Windows, because there may not
	 * be proper support for Unix-y file permissions.  Need to think of a
	 * reasonable check to apply on Windows.
	 */
#if !defined(__CYGWIN__) && !defined(WIN32)
	if (stat_buf.st_mode & (S_IRWXG | S_IRWXO))
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("data directory \"%s\" has group or world access",
						checkdir),
				 errdetail("Permissions should be u=rwx (0700).")));
#endif

	/* Look for PG_VERSION before looking for pg_control */
	ValidatePgVersion(checkdir);

	snprintf(path, sizeof(path), "%s/global/pg_control", checkdir);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		fprintf(stderr,
				gettext("%s: could not find the database system\n"
						"Expected to find it in the directory \"%s\",\n"
						"but could not open file \"%s\": %s\n"),
				progname, checkdir, path, strerror(errno));
		ExitPostmaster(2);
	}
	FreeFile(fp);
}


#ifdef USE_RENDEZVOUS

/* reg_reply -- empty callback function for DNSServiceRegistrationCreate() */
static void
reg_reply(DNSServiceRegistrationReplyErrorType errorCode, void *context)
{

}
#endif

int
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char		original_extraoptions[MAXPGPATH];
	char	   *potential_DataDir = NULL;
	int			i;

	*original_extraoptions = '\0';

	progname = argv[0];

	IsPostmasterEnvironment = true;

	/*
	 * Catch standard options before doing much else.  This even works on
	 * systems without getopt_long.
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			ExitPostmaster(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
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
	 * Fire up essential subsystems: memory management
	 */
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

	IgnoreSystemIndexes(false);

	/*
	 * Options setup
	 */
	InitializeGUCOptions();

	potential_DataDir = getenv("PGDATA");		/* default value */

	opterr = 1;

	while ((opt = getopt(argc, argv, "A:a:B:b:c:D:d:Fh:ik:lm:MN:no:p:Ss-:")) != -1)
	{
		switch (opt)
		{
			case 'A':
#ifdef USE_ASSERT_CHECKING
				SetConfigOption("debug_assertions", optarg, PGC_POSTMASTER, PGC_S_ARGV);
#else
				postmaster_error("assert checking is not compiled in");
#endif
				break;
			case 'a':
				/* Can no longer set authentication method. */
				break;
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'b':
				/* Can no longer set the backend executable file to use. */
				break;
			case 'D':
				potential_DataDir = optarg;
				break;
			case 'd':
				{
					/* Turn on debugging for the postmaster. */
					char	   *debugstr = palloc(strlen("debug") + strlen(optarg) + 1);

					sprintf(debugstr, "debug%s", optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
					debug_flag = atoi(optarg);
					break;
				}
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'h':
				SetConfigOption("virtual_host", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'i':
				SetConfigOption("tcpip_socket", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'k':
				SetConfigOption("unix_socket_directory", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
#ifdef USE_SSL
			case 'l':
				SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
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
				/* The max number of backends to start. */
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
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
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'S':

				/*
				 * Start in 'S'ilent mode (disassociate from controlling
				 * tty). You may also think of this as 'S'ysV mode since
				 * it's most badly needed on SysV-derived systems like
				 * SVR4 and HP-UX.
				 */
				SetConfigOption("silent_mode", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 's':

				/*
				 * In the event that some backend dumps core, send
				 * SIGSTOP, rather than SIGQUIT, to all its peers.	This
				 * lets the wily post_hacker collect core dumps from
				 * everyone.
				 */
				SendStop = true;
				break;
			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (opt == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					free(name);
					if (value)
						free(value);
					break;
				}

			default:
				fprintf(stderr,
					  gettext("Try \"%s --help\" for more information.\n"),
						progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster accepts no non-option switch arguments.
	 */
	if (optind < argc)
	{
		postmaster_error("invalid argument: \"%s\"", argv[optind]);
		fprintf(stderr,
				gettext("Try \"%s --help\" for more information.\n"),
				progname);
		ExitPostmaster(1);
	}

	/*
	 * Now we can set the data directory, and then read postgresql.conf.
	 */
	checkDataDir(potential_DataDir);	/* issues error messages */
	SetDataDir(potential_DataDir);

	ProcessConfigFile(PGC_POSTMASTER);
#ifdef EXEC_BACKEND
	write_nondefault_variables(PGC_POSTMASTER);
#endif

	/*
	 * Check for invalid combinations of GUC settings.
	 */
	if (NBuffers < 2 * MaxBackends || NBuffers < 16)
	{
		/*
		 * Do not accept -B so small that backends are likely to starve
		 * for lack of buffers.  The specific choices here are somewhat
		 * arbitrary.
		 */
		postmaster_error("the number of buffers (-B) must be at least twice the number of allowed connections (-N) and at least 16");
		ExitPostmaster(1);
	}

	if (ReservedBackends >= MaxBackends)
	{
		postmaster_error("superuser_reserved_connections must be less than max_connections");
		ExitPostmaster(1);
	}

	/*
	 * Other one-time internal sanity checks can go here.
	 */
	if (!CheckDateTokenTables())
	{
		postmaster_error("invalid datetoken tables, please fix");
		ExitPostmaster(1);
	}

	/*
	 * Now that we are done processing the postmaster arguments, reset
	 * getopt(3) library so that it will work correctly in subprocesses.
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* some systems need this too */
#endif

	/* For debugging: display postmaster environment */
	{
		extern char **environ;
		char	  **p;

		ereport(DEBUG3,
				(errmsg_internal("%s: PostmasterMain: initial environ dump:",
								 progname)));
		ereport(DEBUG3,
				(errmsg_internal("-----------------------------------------")));
		for (p = environ; *p; ++p)
			ereport(DEBUG3,
					(errmsg_internal("\t%s", *p)));
		ereport(DEBUG3,
				(errmsg_internal("-----------------------------------------")));
	}

	/*
	 * On some systems our dynloader code needs the executable's pathname.
	 */
	if (FindExec(pg_pathname, progname, "postgres") < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate postgres executable",
						progname)));

	/*
	 * Initialize SSL library, if specified.
	 */
#ifdef USE_SSL
	if (EnableSSL && !NetServer)
	{
		postmaster_error("TCP/IP connections must be enabled for SSL");
		ExitPostmaster(1);
	}
	if (EnableSSL)
		secure_initialize();
#endif

	/*
	 * process any libraries that should be preloaded and optionally
	 * pre-initialized
	 */
	if (preload_libraries_string)
		process_preload_libraries(preload_libraries_string);

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
	 * interlock (thanks to whoever decided to put socket files in /tmp
	 * :-(). For the same reason, it's best to grab the TCP socket before
	 * the Unix socket.
	 */
	CreateDataDirLockFile(DataDir, true);

	/*
	 * Remove old temporary files.	At this point there can be no other
	 * Postgres processes running in this directory, so this should be
	 * safe.
	 */
	RemovePgTempFiles();

	/*
	 * Establish input sockets.
	 */
	for (i = 0; i < MAXLISTEN; i++)
		ListenSocket[i] = -1;

	if (NetServer)
	{
		if (VirtualHost && VirtualHost[0])
		{
			char	   *curhost,
					   *endptr;
			char		c = 0;

			curhost = VirtualHost;
			for (;;)
			{
				while (*curhost == ' ') /* skip any extra spaces */
					curhost++;
				if (*curhost == '\0')
					break;
				endptr = strchr(curhost, ' ');
				if (endptr)
				{
					c = *endptr;
					*endptr = '\0';
				}
				status = StreamServerPort(AF_UNSPEC, curhost,
										  (unsigned short) PostPortNumber,
										  UnixSocketDir,
										  ListenSocket, MAXLISTEN);
				if (status != STATUS_OK)
					ereport(FATAL,
					 (errmsg("could not create listen socket for \"%s\"",
							 curhost)));
				if (endptr)
				{
					*endptr = c;
					curhost = endptr + 1;
				}
				else
					break;
			}
		}
		else
		{
			status = StreamServerPort(AF_UNSPEC, NULL,
									  (unsigned short) PostPortNumber,
									  UnixSocketDir,
									  ListenSocket, MAXLISTEN);
			if (status != STATUS_OK)
				ereport(FATAL,
					  (errmsg("could not create TCP/IP listen socket")));
		}

#ifdef USE_RENDEZVOUS
		if (rendezvous_name != NULL)
		{
			DNSServiceRegistrationCreate(rendezvous_name,
										 "_postgresql._tcp.",
										 "",
										 htons(PostPortNumber),
										 "",
								 (DNSServiceRegistrationReply) reg_reply,
										 NULL);
		}
#endif
	}

#ifdef HAVE_UNIX_SOCKETS
	status = StreamServerPort(AF_UNIX, NULL,
							  (unsigned short) PostPortNumber,
							  UnixSocketDir,
							  ListenSocket, MAXLISTEN);
	if (status != STATUS_OK)
		ereport(FATAL,
				(errmsg("could not create Unix-domain socket")));
#endif

	XLOGPathInit();

	/*
	 * Set up shared memory and semaphores.
	 */
	reset_shared(PostPortNumber);

	/*
	 * Estimate number of openable files.  This must happen after setting up
	 * semaphores, because on some platforms semaphores count as open files.
	 */
	set_max_safe_fds();

	/*
	 * Initialize the list of active backends.
	 */
	BackendList = DLNewList();

	/*
	 * Record postmaster options.  We delay this till now to avoid
	 * recording bogus options (eg, NBuffers too high for available
	 * memory).
	 */
	if (!CreateOptsFile(argc, argv))
		ExitPostmaster(1);

	/*
	 * Set up signal handlers for the postmaster process.
	 *
	 * CAUTION: when changing this list, check for side-effects on the signal
	 * handling setup of child processes.  See tcop/postgres.c,
	 * bootstrap/bootstrap.c, and postmaster/pgstat.c.
	 */
	pqinitmask();
	PG_SETMASK(&BlockSig);

	pqsignal(SIGHUP, SIGHUP_handler);	/* reread config file and have
										 * children do same */
	pqsignal(SIGINT, pmdie);	/* send SIGTERM and ShutdownDataBase */
	pqsignal(SIGQUIT, pmdie);	/* send SIGQUIT and die */
	pqsignal(SIGTERM, pmdie);	/* wait for children and ShutdownDataBase */
	pqsignal(SIGALRM, SIG_IGN); /* ignored */
	pqsignal(SIGPIPE, SIG_IGN); /* ignored */
	pqsignal(SIGUSR1, sigusr1_handler); /* message from child process */
	pqsignal(SIGUSR2, dummy_handler);	/* unused, reserve for children */
	pqsignal(SIGCHLD, reaper);	/* handle child termination */
	pqsignal(SIGTTIN, SIG_IGN); /* ignored */
	pqsignal(SIGTTOU, SIG_IGN); /* ignored */
	/* ignore SIGXFSZ, so that ulimit violations work like disk full */
#ifdef SIGXFSZ
	pqsignal(SIGXFSZ, SIG_IGN); /* ignored */
#endif

	/*
	 * Reset whereToSendOutput from Debug (its starting state) to None.
	 * This prevents ereport from sending log messages to stderr unless
	 * the syslog/stderr switch permits.  We don't do this until the
	 * postmaster is fully launched, since startup failures may as well be
	 * reported to stderr.
	 */
	whereToSendOutput = None;

	/*
	 * On many platforms, the first call of localtime() incurs significant
	 * overhead to load timezone info from the system configuration files.
	 * By doing it once in the postmaster, we avoid having to do it in
	 * every started child process.  The savings are not huge, but they
	 * add up...
	 */
	{
		time_t		now = time(NULL);

		(void) localtime(&now);
	}

	/*
	 * Initialize and try to startup the statistics collector process
	 */
	pgstat_init();
	pgstat_start();

	/*
	 * Load cached files for client authentication.
	 */
	load_hba();
	load_ident();
	load_user();
	load_group();

	/*
	 * We're ready to rock and roll...
	 */
	StartupPID = StartupDataBase();

	status = ServerLoop();

	/*
	 * ServerLoop probably shouldn't ever return, but if it does, close
	 * down.
	 */
	ExitPostmaster(status != STATUS_OK);

	return 0;					/* not reached */
}

static void
pmdaemonize(int argc, char *argv[])
{
	int			i;
	pid_t		pid;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

#ifdef LINUX_PROFILE
	/* see comments in BackendStartup */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

	pid = fork();
	if (pid == (pid_t) -1)
	{
		postmaster_error("could not fork background process: %s",
						 strerror(errno));
		ExitPostmaster(1);
	}
	else if (pid)
	{							/* parent */
		/* Parent should just exit, without doing any atexit cleanup */
		_exit(0);
	}

#ifdef LINUX_PROFILE
	setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

	MyProcPid = getpid();		/* reset MyProcPid to child */

/* GH: If there's no setsid(), we hopefully don't need silent mode.
 * Until there's a better solution.
 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		postmaster_error("could not dissociate from controlling TTY: %s",
						 strerror(errno));
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
	printf(gettext("%s is the PostgreSQL server.\n\n"), progname);
	printf(gettext("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(gettext("Options:\n"));
#ifdef USE_ASSERT_CHECKING
	printf(gettext("  -A 1|0          enable/disable run-time assert checking\n"));
#endif
	printf(gettext("  -B NBUFFERS     number of shared buffers\n"));
	printf(gettext("  -c NAME=VALUE   set run-time parameter\n"));
	printf(gettext("  -d 1-5          debugging level\n"));
	printf(gettext("  -D DATADIR      database directory\n"));
	printf(gettext("  -F              turn fsync off\n"));
	printf(gettext("  -h HOSTNAME     host name or IP address to listen on\n"));
	printf(gettext("  -i              enable TCP/IP connections\n"));
	printf(gettext("  -k DIRECTORY    Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(gettext("  -l              enable SSL connections\n"));
#endif
	printf(gettext("  -N MAX-CONNECT  maximum number of allowed connections\n"));
	printf(gettext("  -o OPTIONS      pass \"OPTIONS\" to each server process\n"));
	printf(gettext("  -p PORT         port number to listen on\n"));
	printf(gettext("  -S              silent mode (start in background without logging output)\n"));
	printf(gettext("  --help          show this help, then exit\n"));
	printf(gettext("  --version       output version information, then exit\n"));

	printf(gettext("\nDeveloper options:\n"));
	printf(gettext("  -n              do not reinitialize shared memory after abnormal exit\n"));
	printf(gettext("  -s              send SIGSTOP to all backend servers if one dies\n"));

	printf(gettext("\nPlease read the documentation for the complete list of run-time\n"
				   "configuration settings and how to set them on the command line or in\n"
				   "the configuration file.\n\n"
				   "Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static int
ServerLoop(void)
{
	fd_set		readmask;
	int			nSockets;
	struct timeval now,
				later;
	struct timezone tz;
	int			i;

	gettimeofday(&now, &tz);

	nSockets = initMasks(&readmask);

	for (;;)
	{
		Port	   *port;
		fd_set		rmask;
		struct timeval timeout;

		/*
		 * The timeout for the select() below is normally set on the basis
		 * of the time to the next checkpoint.	However, if for some
		 * reason we don't have a next-checkpoint time, time out after 60
		 * seconds. This keeps checkpoint scheduling from locking up when
		 * we get new connection requests infrequently (since we are
		 * likely to detect checkpoint completion just after enabling
		 * signals below, after we've already made the decision about how
		 * long to wait this time).
		 */
		timeout.tv_sec = 60;
		timeout.tv_usec = 0;

		if (CheckPointPID == 0 && checkpointed &&
			Shutdown == NoShutdown && !FatalError && random_seed != 0)
		{
			time_t		now = time(NULL);

			if (CheckPointTimeout + checkpointed > now)
			{
				/*
				 * Not time for checkpoint yet, so set select timeout
				 */
				timeout.tv_sec = CheckPointTimeout + checkpointed - now;
			}
			else
			{
				/* Time to make the checkpoint... */
				CheckPointPID = CheckPointDataBase();

				/*
				 * if fork failed, schedule another try at 0.1 normal
				 * delay
				 */
				if (CheckPointPID == 0)
				{
					timeout.tv_sec = CheckPointTimeout / 10;
					checkpointed = now + timeout.tv_sec - CheckPointTimeout;
				}
			}
		}

		/*
		 * Wait for something to happen.
		 */
		memcpy((char *) &rmask, (char *) &readmask, sizeof(fd_set));

		PG_SETMASK(&UnBlockSig);

		if (select(nSockets, &rmask, (fd_set *) NULL,
				   (fd_set *) NULL, &timeout) < 0)
		{
			PG_SETMASK(&BlockSig);
			if (errno == EINTR || errno == EWOULDBLOCK)
				continue;
			ereport(LOG,
					(errcode_for_socket_access(),
					 errmsg("select() failed in postmaster: %m")));
			return STATUS_ERROR;
		}

		/*
		 * Block all signals until we wait again.  (This makes it safe for
		 * our signal handlers to do nontrivial work.)
		 */
		PG_SETMASK(&BlockSig);

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
		 * New connection pending on any of our sockets? If so, fork a
		 * child process to deal with it.
		 */
		for (i = 0; i < MAXLISTEN; i++)
		{
			if (ListenSocket[i] == -1)
				break;
			if (FD_ISSET(ListenSocket[i], &rmask))
			{
				port = ConnCreate(ListenSocket[i]);
				if (port)
				{
					BackendStartup(port);

					/*
					 * We no longer need the open socket or port structure
					 * in this process
					 */
					StreamClose(port->sock);
					ConnFree(port);
				}
			}
		}

		/* If we have lost the stats collector, try to start a new one */
		if (!pgstat_is_running)
			pgstat_start();
	}
}


/*
 * Initialise the masks for select() for the ports
 * we are listening on.  Return the number of sockets to listen on.
 */

static int
initMasks(fd_set *rmask)
{
	int			nsocks = -1;
	int			i;

	FD_ZERO(rmask);

	for (i = 0; i < MAXLISTEN; i++)
	{
		int			fd = ListenSocket[i];

		if (fd == -1)
			break;
		FD_SET(fd, rmask);
		if (fd > nsocks)
			nsocks = fd;
	}

	return nsocks + 1;
}


/*
 * Read the startup packet and do something according to it.
 *
 * Returns STATUS_OK or STATUS_ERROR, or might call ereport(FATAL) and
 * not return at all.
 *
 * (Note that ereport(FATAL) stuff is sent to the client, so only use it
 * if that's what you want.  Return STATUS_ERROR if you don't want to
 * send anything to the client, which would typically be appropriate
 * if we detect a communications failure.)
 */
static int
ProcessStartupPacket(Port *port, bool SSLdone)
{
	enum CAC_state cac;
	int32		len;
	void	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

	if (pq_getbytes((char *) &len, 4) == EOF)
	{
		/*
		 * EOF after SSLdone probably means the client didn't like our
		 * response to NEGOTIATE_SSL_CODE.	That's not an error condition,
		 * so don't clutter the log with a complaint.
		 */
		if (!SSLdone)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = ntohl(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * Allocate at least the size of an old-style startup packet, plus one
	 * extra byte, and make sure all are zeroes.  This ensures we will
	 * have null termination of all strings, in both fixed- and
	 * variable-length packet layouts.
	 */
	if (len <= (int32) sizeof(StartupPacket))
		buf = palloc0(sizeof(StartupPacket) + 1);
	else
		buf = palloc0(len + 1);

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * The first field is either a protocol version number or a special
	 * request code.
	 */
	port->proto = proto = ntohl(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		processCancelRequest(port, buf);
		return 127;				/* XXX */
	}

	if (proto == NEGOTIATE_SSL_CODE && !SSLdone)
	{
		char		SSLok;

#ifdef USE_SSL
		/* No SSL when disabled or on Unix sockets */
		if (!EnableSSL || IS_AF_UNIX(port->laddr.addr.ss_family))
			SSLok = 'N';
		else
			SSLok = 'S';		/* Support for SSL */
#else
		SSLok = 'N';			/* No support for SSL */
#endif
		if (send(port->sock, &SSLok, 1, 0) != 1)
		{
			ereport(COMMERROR,
					(errcode_for_socket_access(),
				 errmsg("failed to send SSL negotiation response: %m")));
			return STATUS_ERROR;	/* close the connection */
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif
		/* regular startup packet, cancel, etc packet should follow... */
		/* but not another SSL negotiation request */
		return ProcessStartupPacket(port, true);
	}

	/* Could add additional special packet types here */

	/*
	 * Set FrontendProtocol now so that ereport() knows what format to
	 * send if we fail during startup.
	 */
	FrontendProtocol = proto;

	/* Check we can handle the protocol the frontend is using. */

	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
	  PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) ||
	(PG_PROTOCOL_MAJOR(proto) == PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST) &&
	 PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST)))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
					  PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * Now fetch parameters out of startup packet and save them into the
	 * Port structure.	All data structures attached to the Port struct
	 * must be allocated in TopMemoryContext so that they won't disappear
	 * when we pass them to PostgresMain (see BackendFork).  We need not
	 * worry about leaking this storage on failure, since we aren't in the
	 * postmaster process anymore.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (PG_PROTOCOL_MAJOR(proto) >= 3)
	{
		int32		offset = sizeof(ProtocolVersion);

		/*
		 * Scan packet body for name/option pairs.	We can assume any
		 * string beginning within the packet body is null-terminated,
		 * thanks to zeroing extra byte above.
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = ((char *) buf) + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* found packet terminator */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* missing value, will complain below */
			valptr = ((char *) buf) + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else
			{
				/* Assume it's a generic GUC option */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * If we didn't find a packet terminator exactly at the end of the
		 * given packet length, complain.
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));
	}
	else
	{
		/*
		 * Get the parameters from the old-style, fixed-width-fields
		 * startup packet as C strings.  The packet destination was
		 * cleared first so a short packet has zeros silently added.  We
		 * have to be prepared to truncate the pstrdup result for oversize
		 * fields, though.
		 */
		StartupPacket *packet = (StartupPacket *) buf;

		port->database_name = pstrdup(packet->database);
		if (strlen(port->database_name) > sizeof(packet->database))
			port->database_name[sizeof(packet->database)] = '\0';
		port->user_name = pstrdup(packet->user);
		if (strlen(port->user_name) > sizeof(packet->user))
			port->user_name[sizeof(packet->user)] = '\0';
		port->cmdline_options = pstrdup(packet->options);
		if (strlen(port->cmdline_options) > sizeof(packet->options))
			port->cmdline_options[sizeof(packet->options)] = '\0';
		port->guc_options = NIL;
	}

	/* Check a user name was given. */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
		 errmsg("no PostgreSQL user name specified in startup packet")));

	/* The database defaults to the user name. */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	if (Db_user_namespace)
	{
		/*
		 * If user@, it is a global user, remove '@'. We only want to do
		 * this if there is an '@' at the end and no earlier in the user
		 * string or they may fake as a local user of another database
		 * attaching to this database.
		 */
		if (strchr(port->user_name, '@') ==
			port->user_name + strlen(port->user_name) - 1)
			*strchr(port->user_name, '@') = '\0';
		else
		{
			/* Append '@' and dbname */
			char	   *db_user;

			db_user = palloc(strlen(port->user_name) +
							 strlen(port->database_name) + 2);
			sprintf(db_user, "%s@%s", port->user_name, port->database_name);
			port->user_name = db_user;
		}
	}

	/*
	 * Truncate given database and user names to length of a Postgres
	 * name.  This avoids lookup failures when overlength names are given.
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	/*
	 * Done putting stuff in TopMemoryContext.
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * If we're going to reject the connection due to database state, say
	 * so now instead of wasting cycles on an authentication exchange.
	 * (This also allows a pg_ping utility to be written.)
	 */
	cac = canAcceptConnections();

	switch (cac)
	{
		case CAC_STARTUP:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is starting up")));
			break;
		case CAC_SHUTDOWN:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is shutting down")));
			break;
		case CAC_RECOVERY:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is in recovery mode")));
			break;
		case CAC_TOOMANY:
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
			break;
		case CAC_OK:
		default:
			break;
	}

	return STATUS_OK;
}


/*
 * The client has sent a cancel request packet, not a normal
 * start-a-new-connection packet.  Perform the necessary processing.
 * Nothing is sent back to the client.
 */
static void
processCancelRequest(Port *port, void *pkt)
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
		ereport(DEBUG2,
				(errmsg_internal("ignoring cancel request for checkpoint process %d",
								 backendPID)));
		return;
	}
	else if (ExecBackend)
		AttachSharedMemoryAndSemaphores();

	/* See if we have a matching backend */

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		bp = (Backend *) DLE_VAL(curr);
		if (bp->pid == backendPID)
		{
			if (bp->cancel_key == cancelAuthCode)
			{
				/* Found a match; signal that backend to cancel current op */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));
				kill(bp->pid, SIGINT);
			}
			else
				/* Right PID, wrong key: no way, Jose */
				ereport(DEBUG2,
						(errmsg_internal("bad key in cancel request for process %d",
										 backendPID)));
			return;
		}
	}

	/* No matching backend */
	ereport(DEBUG2,
			(errmsg_internal("bad pid in cancel request for process %d",
							 backendPID)));
}

/*
 * canAcceptConnections --- check to see if database state allows connections.
 */
static enum CAC_state
canAcceptConnections(void)
{
	/* Can't start backends when in startup/shutdown/recovery state. */
	if (Shutdown > NoShutdown)
		return CAC_SHUTDOWN;
	if (StartupPID)
		return CAC_STARTUP;
	if (FatalError)
		return CAC_RECOVERY;

	/*
	 * Don't start too many children.
	 *
	 * We allow more connections than we can have backends here because some
	 * might still be authenticating; they might fail auth, or some
	 * existing backend might exit before the auth cycle is completed. The
	 * exact MaxBackends limit is enforced when a new backend tries to
	 * join the shared-inval backend array.
	 */
	if (CountChildren() >= 2 * MaxBackends)
		return CAC_TOOMANY;

	return CAC_OK;
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
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
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
		/*
		 * Precompute password salt values to use for this connection.
		 * It's slightly annoying to do this long in advance of knowing
		 * whether we'll need 'em or not, but we must do the random()
		 * calls before we fork, not after.  Else the postmaster's random
		 * sequence won't get advanced, and all backends would end up
		 * using the same salt...
		 */
		RandomSalt(port->cryptSalt, port->md5Salt);
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
	secure_close(conn);
#endif
	free(conn);
}


/*
 * ClosePostmasterPorts -- close all the postmaster's open sockets
 *
 * This is called during child process startup to release file descriptors
 * that are not needed by that child process.  The postmaster still has
 * them open, of course.
 */
void
ClosePostmasterPorts(bool pgstat_too)
{
	int			i;

	/* Close the listen sockets */
	for (i = 0; i < MAXLISTEN; i++)
	{
		if (ListenSocket[i] != -1)
		{
			StreamClose(ListenSocket[i]);
			ListenSocket[i] = -1;
		}
	}

	/* Close pgstat control sockets, unless we're starting pgstat itself */
	if (pgstat_too)
		pgstat_close_sockets();
}


/*
 * reset_shared -- reset shared memory and semaphores
 */
static void
reset_shared(unsigned short port)
{
	/*
	 * Create or re-create shared memory and semaphores.
	 *
	 * Note: in each "cycle of life" we will normally assign the same IPC
	 * keys (if using SysV shmem and/or semas), since the port number is
	 * used to determine IPC keys.	This helps ensure that we will clean
	 * up dead IPC objects if the postmaster crashes and is restarted.
	 */
	CreateSharedMemoryAndSemaphores(false, MaxBackends, port);
}


/*
 * SIGHUP -- reread config files, and tell children to do same
 */
static void
SIGHUP_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
			 (errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
#ifdef EXEC_BACKEND
		write_nondefault_variables(PGC_SIGHUP);
#endif
		SignalChildren(SIGHUP);
		load_hba();
		load_ident();
	}

	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}



/*
 * pmdie -- signal handler for processing various postmaster signals.
 */
static void
pmdie(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	ereport(DEBUG2,
			(errmsg_internal("postmaster received signal %d",
							 postgres_signal_arg)));

	switch (postgres_signal_arg)
	{
		case SIGTERM:

			/*
			 * Smart Shutdown:
			 *
			 * Wait for children to end their work and ShutdownDataBase.
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));
			if (DLGetHead(BackendList)) /* let reaper() handle this */
				break;

			/*
			 * No children left. Shutdown data base system.
			 */
			if (StartupPID > 0 || FatalError)	/* let reaper() handle
												 * this */
				break;
			if (ShutdownPID > 0)
			{
				elog(PANIC, "shutdown process %d already running",
					 (int) ShutdownPID);
				abort();
			}

			ShutdownPID = ShutdownDataBase();
			break;

		case SIGINT:

			/*
			 * Fast Shutdown:
			 *
			 * abort all children with SIGTERM (rollback active transactions
			 * and exit) and ShutdownDataBase when they are gone.
			 */
			if (Shutdown >= FastShutdown)
				break;
			ereport(LOG,
					(errmsg("received fast shutdown request")));
			if (DLGetHead(BackendList)) /* let reaper() handle this */
			{
				Shutdown = FastShutdown;
				if (!FatalError)
				{
					ereport(LOG,
							(errmsg("aborting any active transactions")));
					SignalChildren(SIGTERM);
				}
				break;
			}
			if (Shutdown > NoShutdown)
			{
				Shutdown = FastShutdown;
				break;
			}
			Shutdown = FastShutdown;

			/*
			 * No children left. Shutdown data base system.
			 */
			if (StartupPID > 0 || FatalError)	/* let reaper() handle
												 * this */
				break;
			if (ShutdownPID > 0)
			{
				elog(PANIC, "shutdown process %d already running",
					 (int) ShutdownPID);
				abort();
			}

			ShutdownPID = ShutdownDataBase();
			break;

		case SIGQUIT:

			/*
			 * Immediate Shutdown:
			 *
			 * abort all children with SIGQUIT and exit without attempt to
			 * properly shutdown data base system.
			 */
			ereport(LOG,
					(errmsg("received immediate shutdown request")));
			if (ShutdownPID > 0)
				kill(ShutdownPID, SIGQUIT);
			if (StartupPID > 0)
				kill(StartupPID, SIGQUIT);
			if (DLGetHead(BackendList))
				SignalChildren(SIGQUIT);
			ExitPostmaster(0);
			break;
	}

	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}

/*
 * Reaper -- signal handler to cleanup after a backend (child) dies.
 */
static void
reaper(SIGNAL_ARGS)
{
	int			save_errno = errno;

#ifdef WIN32
#warning fix waidpid for Win32
#else
#ifdef HAVE_WAITPID
	int			status;			/* backend exit status */

#else
	union wait	status;			/* backend exit status */
#endif
	int			exitstatus;
	int			pid;			/* process id of dead backend */

	PG_SETMASK(&BlockSig);

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));
#ifdef HAVE_WAITPID
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		exitstatus = status;
#else
	while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
	{
		exitstatus = status.w_status;
#endif

		/*
		 * Check if this child was the statistics collector. If so, try to
		 * start a new one.  (If fail, we'll try again in future cycles of
		 * the main loop.)
		 */
		if (pgstat_ispgstat(pid))
		{
			LogChildExit(LOG, gettext("statistics collector process"),
						 pid, exitstatus);
			pgstat_start();
			continue;
		}

		/*
		 * Check if this child was a shutdown or startup process.
		 */
		if (ShutdownPID > 0 && pid == ShutdownPID)
		{
			if (exitstatus != 0)
			{
				LogChildExit(LOG, gettext("shutdown process"),
							 pid, exitstatus);
				ExitPostmaster(1);
			}
			/* Normal postmaster exit is here */
			ExitPostmaster(0);
		}

		if (StartupPID > 0 && pid == StartupPID)
		{
			if (exitstatus != 0)
			{
				LogChildExit(LOG, gettext("startup process"),
							 pid, exitstatus);
				ereport(LOG,
						(errmsg("aborting startup due to startup process failure")));
				ExitPostmaster(1);
			}
			StartupPID = 0;

			/*
			 * Startup succeeded - remember its ID and RedoRecPtr.
			 *
			 * NB: this MUST happen before we fork a checkpoint or shutdown
			 * subprocess, else they will have wrong local ThisStartUpId.
			 */
			SetThisStartUpID();

			FatalError = false; /* done with recovery */

			/*
			 * Arrange for first checkpoint to occur after standard delay.
			 */
			CheckPointPID = 0;
			checkpointed = time(NULL);

			/*
			 * Go to shutdown mode if a shutdown request was pending.
			 */
			if (Shutdown > NoShutdown)
			{
				if (ShutdownPID > 0)
				{
					elog(PANIC, "startup process %d died while shutdown process %d already running",
						 pid, (int) ShutdownPID);
					abort();
				}
				ShutdownPID = ShutdownDataBase();
			}

			goto reaper_done;
		}

		/*
		 * Else do standard child cleanup.
		 */
		CleanupProc(pid, exitstatus);

	}							/* loop over pending child-death reports */
#endif

	if (FatalError)
	{
		/*
		 * Wait for all children exit, then reset shmem and
		 * StartupDataBase.
		 */
		if (DLGetHead(BackendList) || StartupPID > 0 || ShutdownPID > 0)
			goto reaper_done;
		ereport(LOG,
			(errmsg("all server processes terminated; reinitializing")));

		shmem_exit(0);
		reset_shared(PostPortNumber);

		StartupPID = StartupDataBase();

		goto reaper_done;
	}

	if (Shutdown > NoShutdown)
	{
		if (DLGetHead(BackendList))
			goto reaper_done;
		if (StartupPID > 0 || ShutdownPID > 0)
			goto reaper_done;
		ShutdownPID = ShutdownDataBase();
	}

reaper_done:
	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}

/*
 * CleanupProc -- cleanup after terminated backend.
 *
 * Remove all local state associated with backend.
 */
static void
CleanupProc(int pid,
			int exitstatus)		/* child's exit status. */
{
	Dlelem	   *curr,
			   *next;
	Backend    *bp;

	LogChildExit(DEBUG2, gettext("child process"), pid, exitstatus);

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
				/* Update RedoRecPtr for future child backends */
				GetSavedRedoRecPtr();
			}
		}
		else
			pgstat_beterm(pid);

		return;
	}

	/* below here we're dealing with a non-normal exit */

	/* Make log entry unless we did so already */
	if (!FatalError)
	{
		LogChildExit(LOG,
				 (pid == CheckPointPID) ? gettext("checkpoint process") :
					 gettext("server process"),
					 pid, exitstatus);
		ereport(LOG,
			  (errmsg("terminating any other active server processes")));
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
			 * SIGQUIT is the special signal that says exit without proc_exit
			 * and let the user know what's going on. But if SendStop is
			 * set (-s on command line), then we send SIGSTOP instead, so
			 * that we can get core dumps from all backends by hand.
			 */
			if (!FatalError)
			{
				ereport(DEBUG2,
						(errmsg_internal("sending %s to process %d",
										 (SendStop ? "SIGSTOP" : "SIGQUIT"),
										 (int) bp->pid)));
				kill(bp->pid, (SendStop ? SIGSTOP : SIGQUIT));
			}
		}
		else
		{
			/*
			 * Found entry for freshly-dead backend, so remove it.
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
	else
	{
		/*
		 * Tell the collector about backend termination
		 */
		pgstat_beterm(pid);
	}

	FatalError = true;
}

/*
 * Log the death of a child process.
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*
		 * translator: %s is a noun phrase describing a child process,
		 * such as "server process"
		 */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus))));
	else if (WIFSIGNALED(exitstatus))
		ereport(lev,

		/*
		 * translator: %s is a noun phrase describing a child process,
		 * such as "server process"
		 */
				(errmsg("%s (PID %d) was terminated by signal %d",
						procname, pid, WTERMSIG(exitstatus))));
	else
		ereport(lev,

		/*
		 * translator: %s is a noun phrase describing a child process,
		 * such as "server process"
		 */
				(errmsg("%s (PID %d) exited with unexpected status %d",
						procname, pid, exitstatus)));
}

/*
 * Send a signal to all backend children.
 */
static void
SignalChildren(int signal)
{
	Dlelem	   *curr,
			   *next;
	Backend    *bp;

	curr = DLGetHead(BackendList);
	while (curr)
	{
		next = DLGetSucc(curr);
		bp = (Backend *) DLE_VAL(curr);

		if (bp->pid != MyProcPid)
		{
			ereport(DEBUG2,
					(errmsg_internal("sending signal %d to process %d",
									 signal,
									 (int) bp->pid)));
			kill(bp->pid, signal);
		}

		curr = next;
	}
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork failed, STATUS_OK otherwise.
 */
static int
BackendStartup(Port *port)
{
	Backend    *bn;				/* for backend cleanup */
	pid_t		pid;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * Compute the cancel key that will be assigned to this backend. The
	 * backend will have its own copy in the forked-off process' value of
	 * MyCancelKey, so that it can transmit the key to the frontend.
	 */
	MyCancelKey = PostmasterRandom();

	/*
	 * Make room for backend data structure.  Better before the fork() so
	 * we can handle failure cleanly.
	 */
	bn = (Backend *) malloc(sizeof(Backend));
	if (!bn)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return STATUS_ERROR;
	}

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

#ifdef LINUX_PROFILE

	/*
	 * Linux's fork() resets the profiling timer in the child process. If
	 * we want to profile child processes then we need to save and restore
	 * the timer setting.  This is a waste of time if not profiling,
	 * however, so only do it if commanded by specific -DLINUX_PROFILE
	 * switch.
	 */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

	pid = fork();

	if (pid == 0)				/* child */
	{
		int			status;

#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

#ifdef __BEOS__
		/* Specific beos backend startup actions */
		beos_backend_startup();
#endif
		free(bn);

		status = BackendFork(port);

		if (status != 0)
			ereport(LOG,
					(errmsg("connection startup failed")));
		proc_exit(status);
	}

	/* in parent, error */
	if (pid < 0)
	{
		int			save_errno = errno;

#ifdef __BEOS__
		/* Specific beos backend startup actions */
		beos_backend_startup_failed();
#endif
		free(bn);
		errno = save_errno;
		ereport(LOG,
			  (errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(port, save_errno);
		return STATUS_ERROR;
	}

	/* in parent, normal */
	ereport(DEBUG2,
			(errmsg_internal("forked new backend, pid=%d socket=%d",
							 (int) pid, port->sock)));

	/*
	 * Everything's been successful, it's safe to add this backend to our
	 * list of backends.
	 */
	bn->pid = pid;
	bn->cancel_key = MyCancelKey;
	DLAddHead(BackendList, DLNewElem(bn));

	return STATUS_OK;
}

/*
 * Try to report backend fork() failure to client before we close the
 * connection.	Since we do not care to risk blocking the postmaster on
 * this connection, we set the connection to non-blocking and try only once.
 *
 * This is grungy special-purpose code; we cannot use backend libpq since
 * it's not up and running.
 */
static void
report_fork_failure_to_client(Port *port, int errnum)
{
	char		buffer[1000];

	/* Format the error message packet (always V2 protocol) */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 gettext("could not fork new process for connection: "),
			 strerror(errnum));

	/* Set port to non-blocking.  Don't do send() if this fails */
	if (FCNTL_NONBLOCK(port->sock) < 0)
		return;

	send(port->sock, buffer, strlen(buffer) + 1, 0);
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
 * BackendFork -- perform authentication, and if successful, set up the
 *		backend's argument list and invoke backend main().
 *
 * This used to perform an execv() but we no longer exec the backend;
 * it's the same executable as the postmaster.
 *
 * returns:
 *		Shouldn't return at all.
 *		If PostgresMain() fails, return status.
 */
static int
BackendFork(Port *port)
{
	char	  **av;
	int			maxac;
	int			ac;
	char		debugbuf[32];
	char		protobuf[32];

#ifdef EXEC_BACKEND
	char		pbuf[NAMEDATALEN + 256];
#endif
	int			i;
	int			status;
	struct timeval now;
	struct timezone tz;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];

	/*
	 * Let's clean up ourselves as the postmaster child
	 */

	IsUnderPostmaster = true;	/* we are a postmaster subprocess now */

	ClientAuthInProgress = true;	/* limit visibility of log messages */

	/* We don't want the postmaster's proc_exit() handlers */
	on_exit_reset();

	/*
	 * Signal handlers setting is moved to tcop/postgres...
	 */

	/* Close the postmaster's other sockets */
	ClosePostmasterPorts(true);

	/* Save port etc. for ps status */
	MyProcPort = port;

	/* Reset MyProcPid to new backend's pid */
	MyProcPid = getpid();

	/*
	 * Initialize libpq and enable reporting of ereport errors to the
	 * client. Must do this now because authentication uses libpq to send
	 * messages.
	 */
	pq_init();					/* initialize libpq to talk to client */
	whereToSendOutput = Remote; /* now safe to ereport to client */

	/*
	 * We arrange for a simple exit(0) if we receive SIGTERM or SIGQUIT
	 * during any client authentication related communication. Otherwise
	 * the postmaster cannot shutdown the database FAST or IMMED cleanly
	 * if a buggy client blocks a backend during authentication.
	 */
	pqsignal(SIGTERM, authdie);
	pqsignal(SIGQUIT, authdie);
	pqsignal(SIGALRM, authdie);
	PG_SETMASK(&AuthBlockSig);

	/*
	 * Get the remote host name and port for logging and status display.
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if (getnameinfo_all(&port->raddr.addr, port->raddr.salen,
						remote_host, sizeof(remote_host),
						remote_port, sizeof(remote_port),
				   (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV))
	{
		getnameinfo_all(&port->raddr.addr, port->raddr.salen,
						remote_host, sizeof(remote_host),
						remote_port, sizeof(remote_port),
						NI_NUMERICHOST | NI_NUMERICSERV);
	}

	if (Log_connections)
		ereport(LOG,
				(errmsg("connection received: host=%s port=%s",
						remote_host, remote_port)));

	if (LogSourcePort)
	{
		/* modify remote_host for use in ps status */
		char		tmphost[NI_MAXHOST];

		snprintf(tmphost, sizeof(tmphost), "%s:%s", remote_host, remote_port);
		StrNCpy(remote_host, tmphost, sizeof(remote_host));
	}

	/*
	 * PreAuthDelay is a debugging aid for investigating problems in the
	 * authentication cycle: it can be set in postgresql.conf to allow
	 * time to attach to the newly-forked backend with a debugger. (See
	 * also the -W backend switch, which we allow clients to pass through
	 * PGOPTIONS, but it is not honored until after authentication.)
	 */
	if (PreAuthDelay > 0)
		sleep(PreAuthDelay);

	/*
	 * Ready to begin client interaction.  We will give up and exit(0)
	 * after a time delay, so that a broken client can't hog a connection
	 * indefinitely.  PreAuthDelay doesn't count against the time limit.
	 */
	if (!enable_sig_alarm(AuthenticationTimeout * 1000, false))
		elog(FATAL, "could not set timer for authorization timeout");

	/*
	 * Receive the startup packet (which might turn out to be a cancel
	 * request packet).
	 */
	status = ProcessStartupPacket(port, false);

	if (status != STATUS_OK)
		return 0;				/* cancel request processed, or error */

	/*
	 * Now that we have the user and database name, we can set the process
	 * title for ps.  It's good to do this as early as possible in
	 * startup.
	 */
	init_ps_display(port->user_name, port->database_name, remote_host);
	set_ps_display("authentication");

	/*
	 * Now perform authentication exchange.
	 */
	ClientAuthentication(port); /* might not return, if failure */

	/*
	 * Done with authentication.  Disable timeout, and prevent
	 * SIGTERM/SIGQUIT again until backend startup is complete.
	 */
	if (!disable_sig_alarm(false))
		elog(FATAL, "could not disable timer for authorization timeout");
	PG_SETMASK(&BlockSig);

	if (Log_connections)
		ereport(LOG,
				(errmsg("connection authorized: user=%s database=%s",
						port->user_name, port->database_name)));

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
	 *
	 * The maximum possible number of commandline arguments that could come
	 * from ExtraOptions or port->cmdline_options is (strlen + 1) / 2; see
	 * split_opts().
	 * ----------------
	 */
	maxac = 10;					/* for fixed args supplied below */
	maxac += (strlen(ExtraOptions) + 1) / 2;
	if (port->cmdline_options)
		maxac += (strlen(port->cmdline_options) + 1) / 2;

	av = (char **) MemoryContextAlloc(TopMemoryContext,
									  maxac * sizeof(char *));
	ac = 0;

	av[ac++] = "postgres";

	/*
	 * Pass the requested debugging level along to the backend.
	 */
	if (debug_flag > 0)
	{
		snprintf(debugbuf, sizeof(debugbuf), "-d%d", debug_flag);
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
	snprintf(protobuf, sizeof(protobuf), "-v%u", port->proto);
	av[ac++] = protobuf;

	/*
	 * Tell the backend it is being called from the postmaster, and which
	 * database to use.  -p marks the end of secure switches.
	 */
	av[ac++] = "-p";
#ifdef EXEC_BACKEND
	Assert(UsedShmemSegID != 0 && UsedShmemSegAddr != NULL);
	/* database name at the end because it might contain commas */
	snprintf(pbuf, NAMEDATALEN + 256, "%d,%d,%d,%p,%s", port->sock, canAcceptConnections(),
			 UsedShmemSegID, UsedShmemSegAddr, port->database_name);
	av[ac++] = pbuf;
#else
	av[ac++] = port->database_name;
#endif

	/*
	 * Pass the (insecure) option switches from the connection request.
	 * (It's OK to mangle port->cmdline_options now.)
	 */
	if (port->cmdline_options)
		split_opts(av, &ac, port->cmdline_options);

	av[ac] = (char *) NULL;

	Assert(ac < maxac);

	/*
	 * Release postmaster's working memory context so that backend can
	 * recycle the space.  Note this does not trash *MyProcPort, because
	 * ConnCreate() allocated that space with malloc() ... else we'd need
	 * to copy the Port data here.	Also, subsidiary data such as the
	 * username isn't lost either; see ProcessStartupPacket().
	 */
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(PostmasterContext);
	PostmasterContext = NULL;

	/*
	 * Debug: print arguments being passed to backend
	 */
	ereport(DEBUG3,
			(errmsg_internal("%s child[%d]: starting with (",
							 progname, MyProcPid)));
	for (i = 0; i < ac; ++i)
		ereport(DEBUG3,
				(errmsg_internal("\t%s", av[i])));
	ereport(DEBUG3,
			(errmsg_internal(")")));

	ClientAuthInProgress = false;		/* client_min_messages is active
										 * now */

	return (PostgresMain(ac, av, port->user_name));
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
	/* Should I use true instead? */
	ClosePostmasterPorts(false);

	proc_exit(status);
}

/*
 * sigusr1_handler - handle signal conditions from child processes
 */
static void
sigusr1_handler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	PG_SETMASK(&BlockSig);

	if (CheckPostmasterSignal(PMSIGNAL_DO_CHECKPOINT))
	{
		if (CheckPointWarning != 0)
		{
			/*
			 * This only times checkpoints forced by running out of
			 * segment files.  Other checkpoints could reduce the
			 * frequency of forced checkpoints.
			 */
			time_t		now = time(NULL);

			if (LastSignalledCheckpoint != 0)
			{
				int			elapsed_secs = now - LastSignalledCheckpoint;

				if (elapsed_secs < CheckPointWarning)
					ereport(LOG,
							(errmsg("checkpoints are occurring too frequently (%d seconds apart)",
									elapsed_secs),
					errhint("Consider increasing the configuration parameter \"checkpoint_segments\".")));
			}
			LastSignalledCheckpoint = now;
		}

		/*
		 * Request to schedule a checkpoint
		 *
		 * Ignore request if checkpoint is already running or checkpointing
		 * is currently disabled
		 */
		if (CheckPointPID == 0 && checkpointed &&
			Shutdown == NoShutdown && !FatalError && random_seed != 0)
		{
			CheckPointPID = CheckPointDataBase();
			/* note: if fork fails, CheckPointPID stays 0; nothing happens */
		}
	}

	if (CheckPostmasterSignal(PMSIGNAL_PASSWORD_CHANGE))
	{
		/*
		 * Password or group file has changed.
		 */
		load_user();
		load_group();
	}

	if (CheckPostmasterSignal(PMSIGNAL_WAKEN_CHILDREN))
	{
		/*
		 * Send SIGUSR2 to all children (triggers AsyncNotifyHandler). See
		 * storage/ipc/sinvaladt.c for the use of this.
		 */
		if (Shutdown == NoShutdown)
			SignalChildren(SIGUSR2);
	}

	PG_SETMASK(&UnBlockSig);

	errno = save_errno;
}


/*
 * Dummy signal handler
 *
 * We use this for signals that we don't actually use in the postmaster,
 * but we do use in backends.  If we SIG_IGN such signals in the postmaster,
 * then a newly started backend might drop a signal that arrives before it's
 * able to reconfigure its signal processing.  (See notes in postgres.c.)
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}


/*
 * CharRemap: given an int in range 0..61, produce textual encoding of it
 * per crypt(3) conventions.
 */
static char
CharRemap(long ch)
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
RandomSalt(char *cryptSalt, char *md5Salt)
{
	long		rand = PostmasterRandom();

	cryptSalt[0] = CharRemap(rand % 62);
	cryptSalt[1] = CharRemap(rand / 62);

	/*
	 * It's okay to reuse the first random value for one of the MD5 salt
	 * bytes, since only one of the two salts will be sent to the client.
	 * After that we need to compute more random bits.
	 *
	 * We use % 255, sacrificing one possible byte value, so as to ensure
	 * that all bits of the random() value participate in the result.
	 * While at it, add one to avoid generating any null bytes.
	 */
	md5Salt[0] = (rand % 255) + 1;
	rand = PostmasterRandom();
	md5Salt[1] = (rand % 255) + 1;
	rand = PostmasterRandom();
	md5Salt[2] = (rand % 255) + 1;
	rand = PostmasterRandom();
	md5Salt[3] = (rand % 255) + 1;
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

	return random();
}

/*
 * Count up number of child processes.
 */
static int
CountChildren(void)
{
	Dlelem	   *curr;
	Backend    *bp;
	int			cnt = 0;

	for (curr = DLGetHead(BackendList); curr; curr = DLGetSucc(curr))
	{
		bp = (Backend *) DLE_VAL(curr);
		if (bp->pid != MyProcPid)
			cnt++;
	}
	if (CheckPointPID != 0)
		cnt--;
	return cnt;
}

/*
 * Fire off a subprocess for startup/shutdown/checkpoint.
 *
 * Return value is subprocess' PID, or 0 if failed to start subprocess
 * (0 is returned only for checkpoint case).
 */
static pid_t
SSDataBase(int xlop)
{
	pid_t		pid;
	Backend    *bn;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	fflush(stdout);
	fflush(stderr);

#ifdef LINUX_PROFILE
	/* see comments in BackendStartup */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

#ifdef __BEOS__
	/* Specific beos actions before backend startup */
	beos_before_backend_startup();
#endif

	if ((pid = fork()) == 0)	/* child */
	{
		const char *statmsg;
		char	   *av[10];
		int			ac = 0;
		char		nbbuf[32];
		char		xlbuf[32];

#ifdef EXEC_BACKEND
		char		pbuf[NAMEDATALEN + 256];
#endif

#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

#ifdef __BEOS__
		/* Specific beos actions after backend startup */
		beos_backend_startup();
#endif

		IsUnderPostmaster = true;		/* we are a postmaster subprocess
										 * now */

		/* Lose the postmaster's on-exit routines and port connections */
		on_exit_reset();

		/* Close the postmaster's sockets */
		ClosePostmasterPorts(true);

		/*
		 * Identify myself via ps
		 */
		switch (xlop)
		{
			case BS_XLOG_STARTUP:
				statmsg = "startup subprocess";
				break;
			case BS_XLOG_CHECKPOINT:
				statmsg = "checkpoint subprocess";
				break;
			case BS_XLOG_SHUTDOWN:
				statmsg = "shutdown subprocess";
				break;
			default:
				statmsg = "??? subprocess";
				break;
		}
		init_ps_display(statmsg, "", "");
		set_ps_display("");

		/* Set up command-line arguments for subprocess */
		av[ac++] = "postgres";

		snprintf(nbbuf, sizeof(nbbuf), "-B%d", NBuffers);
		av[ac++] = nbbuf;

		snprintf(xlbuf, sizeof(xlbuf), "-x%d", xlop);
		av[ac++] = xlbuf;

		av[ac++] = "-p";
#ifdef EXEC_BACKEND
		Assert(UsedShmemSegID != 0 && UsedShmemSegAddr != NULL);
		/* database name at the end because it might contain commas */
		snprintf(pbuf, NAMEDATALEN + 256, "%d,%p,%s", UsedShmemSegID,
				 UsedShmemSegAddr, "template1");
		av[ac++] = pbuf;
#else
		av[ac++] = "template1";
#endif

		av[ac] = (char *) NULL;

		Assert(ac < lengthof(av));

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

		switch (xlop)
		{
			case BS_XLOG_STARTUP:
				ereport(LOG,
						(errmsg("could not fork startup process: %m")));
				break;
			case BS_XLOG_CHECKPOINT:
				ereport(LOG,
					  (errmsg("could not fork checkpoint process: %m")));
				break;
			case BS_XLOG_SHUTDOWN:
				ereport(LOG,
						(errmsg("could not fork shutdown process: %m")));
				break;
			default:
				ereport(LOG,
						(errmsg("could not fork process: %m")));
				break;
		}

		/*
		 * fork failure is fatal during startup/shutdown, but there's no
		 * need to choke if a routine checkpoint fails.
		 */
		if (xlop == BS_XLOG_CHECKPOINT)
			return 0;
		ExitPostmaster(1);
	}

	/*
	 * The startup and shutdown processes are not considered normal
	 * backends, but the checkpoint process is.  Checkpoint must be added
	 * to the list of backends.
	 */
	if (xlop == BS_XLOG_CHECKPOINT)
	{
		if (!(bn = (Backend *) malloc(sizeof(Backend))))
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			ExitPostmaster(1);
		}

		bn->pid = pid;
		bn->cancel_key = PostmasterRandom();
		DLAddHead(BackendList, DLNewElem(bn));

		/*
		 * Since this code is executed periodically, it's a fine place to
		 * do other actions that should happen every now and then on no
		 * particular schedule.  Such as...
		 */
		TouchSocketFile();
		TouchSocketLockFile();
	}

	return pid;
}


/*
 * Create the opts file
 */
static bool
CreateOptsFile(int argc, char *argv[])
{
	char		fullprogname[MAXPGPATH];
	char		filename[MAXPGPATH];
	FILE	   *fp;
	int			i;

	if (FindExec(fullprogname, argv[0], "postmaster") < 0)
		return false;

	snprintf(filename, sizeof(filename), "%s/postmaster.opts", DataDir);

	if ((fp = fopen(filename, "w")) == NULL)
	{
		elog(LOG, "could not create file \"%s\": %m", filename);
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " '%s'", argv[i]);
	fputs("\n", fp);

	fflush(fp);
	if (ferror(fp))
	{
		elog(LOG, "could not write file \"%s\": %m", filename);
		fclose(fp);
		return false;
	}

	fclose(fp);
	return true;
}

/*
 * This should be used only for reporting "interactive" errors (essentially,
 * bogus arguments on the command line).  Once the postmaster is launched,
 * use ereport.  In particular, don't use this for anything that occurs
 * after pmdaemonize.
 */
static void
postmaster_error(const char *fmt,...)
{
	va_list		ap;

	fprintf(stderr, "%s: ", progname);
	va_start(ap, fmt);
	vfprintf(stderr, gettext(fmt), ap);
	va_end(ap);
	fprintf(stderr, "\n");
}
