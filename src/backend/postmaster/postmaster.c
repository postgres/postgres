/*-------------------------------------------------------------------------
 *
 * postmaster.c--
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.	Frontend programs send a startup message
 *	  to the Postmaster and the postmaster uses the info in the
 *	  message to setup a backend process.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/postmaster/postmaster.c,v 1.59 1997/10/25 01:09:55 momjian Exp $
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
 /* moved here to prevent double define */
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#ifdef HAVE_NETDB_H
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 256
#endif

#include "postgres.h"

#include <signal.h>
#include <string.h>
#include <stdlib.h>

#if !defined(NO_UNISTD_H)
#include <unistd.h>
#endif							/* !NO_UNISTD_H */

#include <ctype.h>
#include <sys/types.h>			/* for fd_set stuff */
#include <sys/stat.h>			/* for umask */
#include <sys/time.h>
#include <sys/socket.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#define MAXINT		   INT_MAX
#else
#include <values.h>
#endif
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "storage/ipc.h"
#include "libpq/libpq.h"
#include "libpq/auth.h"
#include "libpq/pqcomm.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "version.h"
#include "lib/dllist.h"
#include "nodes/nodes.h"
#include "utils/mcxt.h"
#include "storage/proc.h"
#include "utils/elog.h"
#include "port-protos.h"		/* For gethostname() */

#if defined(DBX_VERSION)
#define FORK() (0)
#else
#ifndef HAVE_VFORK
#define FORK() fork()
#else
#define FORK() vfork()
#endif
#endif

#define LINGER_TIME 3

 /*
  * Max time in seconds for socket to linger (close() to block) waiting
  * for frontend to retrieve its message from us.
  */

/*
 * Info for garbage collection.  Whenever a process dies, the Postmaster
 * cleans up after it.	Currently, NO information is required for cleanup,
 * but I left this structure around in case that changed.
 */
typedef struct bkend
{
	int			pid;			/* process id of backend */
} Backend;

/* list of active backends.  For garbage collection only now. */

static Dllist *BackendList;

/* list of ports associated with still open, but incomplete connections */
static Dllist *PortList;

static short PostPortName = -1;
static short ActiveBackends = FALSE;
static int	NextBackendId = MAXINT;		/* XXX why? */
static char *progname = (char *) NULL;

/*
 * Default Values
 */
static char Execfile[MAXPATHLEN] = "";

static int	ServerSock = INVALID_SOCK;	/* stream socket server */

/*
 * Set by the -o option
 */
static char ExtraOptions[ARGV_SIZE] = "";

/*
 * These globals control the behavior of the postmaster in case some
 * backend dumps core.	Normally, it kills all peers of the dead backend
 * and reinitializes shared memory.  By specifying -s or -n, we can have
 * the postmaster stop (rather than kill) peers and not reinitialize
 * shared data structures.
 */
static int	Reinit = 1;
static int	SendStop = 0;

static int	MultiplexedBackends = 0;
static int	MultiplexedBackendPort;

/*
 * postmaster.c - function prototypes
 */
static void pmdaemonize(void);
static void
ConnStartup(Port *port, int *status,
			char *errormsg, const int errormsg_len);
static int	ConnCreate(int serverFd, int *newFdP);
static void reset_shared(short port);
static void pmdie(SIGNAL_ARGS);
static void reaper(SIGNAL_ARGS);
static void dumpstatus(SIGNAL_ARGS);
static void CleanupProc(int pid, int exitstatus);
static int	DoExec(StartupInfo *packet, int portFd);
static void ExitPostmaster(int status);
static void usage(const char *);
static int	ServerLoop(void);
static int	BackendStartup(StartupInfo *packet, Port *port, int *pidPtr);
static void send_error_reply(Port *port, const char *errormsg);

extern char *optarg;
extern int	optind,
			opterr;



static void
checkDataDir(const char *DataDir, bool *DataDirOK)
{
	if (DataDir == NULL)
	{
		fprintf(stderr, "%s does not know where to find the database system "
				"data.  You must specify the directory that contains the "
				"database system either by specifying the -D invocation "
			 "option or by setting the PGDATA environment variable.\n\n",
				progname);
		*DataDirOK = false;
	}
	else
	{
		char		path[MAXPATHLEN];
		FILE	   *fp;

		sprintf(path, "%s%cbase%ctemplate1%cpg_class",
				DataDir, SEP_CHAR, SEP_CHAR, SEP_CHAR);
		fp = fopen(path, "r");
		if (fp == NULL)
		{
			fprintf(stderr, "%s does not find the database system.  "
					"Expected to find it "
			   "in the PGDATA directory \"%s\", but unable to open file "
					"with pathname \"%s\".\n\n",
					progname, DataDir, path);
			*DataDirOK = false;
		}
		else
		{
			char	   *reason;

			/* reason ValidatePgVersion failed.  NULL if didn't */

			fclose(fp);

			ValidatePgVersion(DataDir, &reason);
			if (reason)
			{
				fprintf(stderr,
						"Database system in directory %s "
						"is not compatible with this version of "
						"Postgres, or we are unable to read the "
						"PG_VERSION file.  "
						"Explanation from ValidatePgVersion: %s\n\n",
						DataDir, reason);
				free(reason);
				*DataDirOK = false;
			}
			else
				*DataDirOK = true;
		}
	}
}



int
PostmasterMain(int argc, char *argv[])
{
	extern int	NBuffers;		/* from buffer/bufmgr.c */
	extern bool IsPostmaster;	/* from smgr/mm.c */
	int			opt;
	char	   *hostName;
	int			status;
	int			silentflag = 0;
	char		hostbuf[MAXHOSTNAMELEN];
	bool		DataDirOK;		/* We have a usable PGDATA value */

	progname = argv[0];

	IsPostmaster = true;

	/*
	 * for security, no dir or file created can be group or other
	 * accessible
	 */
	umask((mode_t) 0077);

	if (!(hostName = getenv("PGHOST")))
	{
		if (gethostname(hostbuf, MAXHOSTNAMELEN) < 0)
			strcpy(hostbuf, "localhost");
		hostName = hostbuf;
	}

	DataDir = getenv("PGDATA"); /* default value */

	opterr = 0;
	while ((opt = getopt(argc, argv, "a:B:b:D:dm:Mno:p:Ss")) != EOF)
	{
		switch (opt)
		{
			case 'a':
				/* Set the authentication system. */
				be_setauthsvc(optarg);
				break;
			case 'B':

				/*
				 * The number of buffers to create.  Setting this option
				 * means we have to start each backend with a -B # to make
				 * sure they know how many buffers were allocated.
				 */
				NBuffers = atol(optarg);
				strcat(ExtraOptions, " -B ");
				strcat(ExtraOptions, optarg);
				break;
			case 'b':
				/* Set the backend executable file to use. */
				if (!ValidateBackend(optarg))
					strcpy(Execfile, optarg);
				else
				{
					fprintf(stderr, "%s: invalid backend \"%s\"\n",
							progname, optarg);
					exit(2);
				}
				break;
			case 'D':
				/* Set PGDATA from the command line. */
				DataDir = optarg;
				break;
			case 'd':

				/*
				 * Turn on debugging for the postmaster and the backend
				 * servers descended from it.
				 */
				if ((optind < argc) && *argv[optind] != '-')
				{
					DebugLvl = atoi(argv[optind]);
					optind++;
				}
				else
					DebugLvl = 1;
				break;
			case 'm':
				MultiplexedBackends = 1;
				MultiplexedBackendPort = atoi(optarg);
				break;
			case 'M':

				/*
				 * ignore this flag.  This may be passed in because the
				 * program was run as 'postgres -M' instead of
				 * 'postmaster'
				 */
				break;
			case 'n':
				/* Don't reinit shared mem after abnormal exit */
				Reinit = 0;
				break;
			case 'o':

				/*
				 * Other options to pass to the backend on the command
				 * line -- useful only for debugging.
				 */
				strcat(ExtraOptions, " ");
				strcat(ExtraOptions, optarg);
				break;
			case 'p':
				/* Set PGPORT by hand. */
				PostPortName = (short) atoi(optarg);
				break;
			case 'S':

				/*
				 * Start in 'S'ilent mode (disassociate from controlling
				 * tty). You may also think of this as 'S'ysV mode since
				 * it's most badly needed on SysV-derived systems like
				 * SVR4 and HP-UX.
				 */
				silentflag = 1;
				break;
			case 's':

				/*
				 * In the event that some backend dumps core, send
				 * SIGSTOP, rather than SIGUSR1, to all its peers.	This
				 * lets the wily post_hacker collect core dumps from
				 * everyone.
				 */
				SendStop = 1;
				break;
			default:
				/* usage() never returns */
				usage(progname);
				break;
		}
	}
	if (PostPortName == -1)
		PostPortName = pq_getport();

	checkDataDir(DataDir, &DataDirOK);	/* issues error messages */
	if (!DataDirOK)
	{
		fprintf(stderr, "No data directory -- can't proceed.\n");
		exit(2);
	}

	if (!Execfile[0] && FindBackend(Execfile, argv[0]) < 0)
	{
		fprintf(stderr, "%s: could not find backend to execute...\n",
				argv[0]);
		exit(1);
	}


	status = StreamServerPort(hostName, PostPortName, &ServerSock);
	if (status != STATUS_OK)
	{
		fprintf(stderr, "%s: cannot create stream port\n",
				progname);
		exit(1);
	}

	/* set up shared memory and semaphores */
	EnableMemoryContext(TRUE);
	reset_shared(PostPortName);

	/*
	 * Initialize the list of active backends.	This list is only used for
	 * garbage collecting the backend processes.
	 */
	BackendList = DLNewList();
	PortList = DLNewList();

	if (silentflag)
		pmdaemonize();

	pqsignal(SIGINT, pmdie);
	pqsignal(SIGCHLD, reaper);
	pqsignal(SIGTTIN, SIG_IGN);
	pqsignal(SIGTTOU, SIG_IGN);
	pqsignal(SIGHUP, pmdie);
	pqsignal(SIGTERM, pmdie);
	pqsignal(SIGCONT, dumpstatus);
	pqsignal(SIGPIPE, SIG_IGN);

	status = ServerLoop();

	ExitPostmaster(status != STATUS_OK);
	return 0;					/* not reached */
}

static void
pmdaemonize(void)
{
	int			i;

	if (fork())
		exit(0);
/* GH: If there's no setsid(), we hopefully don't need silent mode.
 * Until there's a better solution.
 */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		fprintf(stderr, "%s: ", progname);
		perror("cannot disassociate from controlling TTY");
		exit(1);
	}
#endif
	i = open(NULL_DEV, O_RDWR);
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);
	close(i);
}

static void
usage(const char *progname)
{
	fprintf(stderr, "usage: %s [options..]\n", progname);
	fprintf(stderr, "\t-a authsys\tdo/do not permit use of an authentication system\n");
	fprintf(stderr, "\t-B nbufs\tset number of shared buffers\n");
	fprintf(stderr, "\t-b backend\tuse a specific backend server executable\n");
	fprintf(stderr, "\t-d [1|2|3]\tset debugging level\n");
	fprintf(stderr, "\t-D datadir\tset data directory\n");
	fprintf(stderr, "\t-m \tstart up multiplexing backends\n");
	fprintf(stderr, "\t-n\t\tdon't reinitialize shared memory after abnormal exit\n");
	fprintf(stderr, "\t-o option\tpass 'option' to each backend servers\n");
	fprintf(stderr, "\t-p port\t\tspecify port for postmaster to listen on\n");
	fprintf(stderr, "\t-S\t\tsilent mode (disassociate from tty)\n");
	fprintf(stderr, "\t-s\t\tsend SIGSTOP to all backend servers if one dies\n");
	exit(1);
}

static int
ServerLoop(void)
{
	int			serverFd = ServerSock;
	fd_set		rmask,
				basemask;
	int			nSockets,
				nSelected,
				status,
				newFd;
	Dlelem	   *next,
			   *curr;

	/*
	 * GH: For !HAVE_SIGPROCMASK (NEXTSTEP), TRH implemented an
	 * alternative interface.
	 */
#ifdef HAVE_SIGPROCMASK
	sigset_t	oldsigmask,
				newsigmask;

#else
	int			orgsigmask = sigblock(0);

#endif

	nSockets = ServerSock + 1;
	FD_ZERO(&basemask);
	FD_SET(ServerSock, &basemask);

#ifdef HAVE_SIGPROCMASK
	sigprocmask(0, 0, &oldsigmask);
	sigemptyset(&newsigmask);
	sigaddset(&newsigmask, SIGCHLD);
#endif
	for (;;)
	{
#ifdef HAVE_SIGPROCMASK
		sigprocmask(SIG_SETMASK, &oldsigmask, 0);
#else
		sigsetmask(orgsigmask);
#endif
		newFd = -1;
		memmove((char *) &rmask, (char *) &basemask, sizeof(fd_set));
		if ((nSelected = select(nSockets, &rmask,
								(fd_set *) NULL,
								(fd_set *) NULL,
								(struct timeval *) NULL)) < 0)
		{
			if (errno == EINTR)
				continue;
			fprintf(stderr, "%s: ServerLoop: select failed\n",
					progname);
			return (STATUS_ERROR);
		}

		/*
		 * [TRH] To avoid race conditions, block SIGCHLD signals while we
		 * are handling the request. (both reaper() and ConnCreate()
		 * manipulate the BackEnd list, and reaper() calls free() which is
		 * usually non-reentrant.)
		 */
#ifdef HAVE_SIGPROCMASK
		sigprocmask(SIG_BLOCK, &newsigmask, &oldsigmask);
#else
		sigblock(sigmask(SIGCHLD));		/* XXX[TRH] portability */
#endif
		if (DebugLvl > 1)
		{
			fprintf(stderr, "%s: ServerLoop: %d sockets pending\n",
					progname, nSelected);
		}

		/* new connection pending on our well-known port's socket */
		if (FD_ISSET(ServerSock, &rmask))
		{

			/*
			 * connect and make an addition to PortList.  If the
			 * connection dies and we notice it, just forget about the
			 * whole thing.
			 */
			if (ConnCreate(serverFd, &newFd) == STATUS_OK)
			{
				if (newFd >= nSockets)
					nSockets = newFd + 1;
				FD_SET(newFd, &rmask);
				FD_SET(newFd, &basemask);
				if (DebugLvl)
					fprintf(stderr, "%s: ServerLoop: connect on %d\n",
							progname, newFd);
			}
			--nSelected;
			FD_CLR(ServerSock, &rmask);
		}

		if (DebugLvl > 1)
		{
			fprintf(stderr, "%s: ServerLoop:\tnSelected=%d\n",
					progname, nSelected);
			curr = DLGetHead(PortList);
			while (curr)
			{
				Port	   *port = DLE_VAL(curr);

				fprintf(stderr, "%s: ServerLoop:\t\tport %d%s pending\n",
						progname, port->sock,
						FD_ISSET(port->sock, &rmask)
						? "" :
						" not");
				curr = DLGetSucc(curr);
			}
		}

		curr = DLGetHead(PortList);

		while (curr)
		{
			Port	   *port = (Port *) DLE_VAL(curr);
			int			lastbytes = port->nBytes;

			if (FD_ISSET(port->sock, &rmask) && port->sock != newFd)
			{
				if (DebugLvl > 1)
					fprintf(stderr, "%s: ServerLoop:\t\thandling %d\n",
							progname, port->sock);
				--nSelected;

				/*
				 * Read the incoming packet into its packet buffer. Read
				 * the connection id out of the packet so we know who the
				 * packet is from.
				 */
				status = PacketReceive(port, &port->buf, NON_BLOCKING);
				switch (status)
				{
					case STATUS_OK:
						{
							int			CSstatus;		/* Completion status of
														 * ConnStartup */
							char		errormsg[200];	/* error msg from
														 * ConnStartup */

							ConnStartup(port, &CSstatus, errormsg, sizeof(errormsg));

							if (CSstatus == STATUS_ERROR)
								send_error_reply(port, errormsg);
							ActiveBackends = TRUE;
						}
						/* FALLTHROUGH */
					case STATUS_INVALID:
						if (DebugLvl)
							fprintf(stderr, "%s: ServerLoop:\t\tdone with %d\n",
									progname, port->sock);
						break;
					case STATUS_BAD_PACKET:

						/*
						 * This is a bogus client, kill the connection and
						 * forget the whole thing.
						 */
						if (DebugLvl)
							fprintf(stderr, "%s: ServerLoop:\t\tbad packet format (reported packet size of %d read on port %d\n", progname, port->nBytes, port->sock);
						break;
					case STATUS_NOT_DONE:
						if (DebugLvl)
							fprintf(stderr, "%s: ServerLoop:\t\tpartial packet (%d bytes actually read) on %d\n",
									progname, port->nBytes, port->sock);

						/*
						 * If we've received at least a PacketHdr's worth
						 * of data and we're still receiving data each
						 * time we read, we're ok.  If the client gives us
						 * less than a PacketHdr at the beginning, just
						 * kill the connection and forget about the whole
						 * thing.
						 */
						if (lastbytes < port->nBytes)
						{
							if (DebugLvl)
								fprintf(stderr, "%s: ServerLoop:\t\tpartial packet on %d ok\n",
										progname, port->sock);
							curr = DLGetSucc(curr);
							continue;
						}
						break;
					case STATUS_ERROR:	/* system call error - die */
						fprintf(stderr, "%s: ServerLoop:\t\terror receiving packet\n",
								progname);
						return (STATUS_ERROR);
				}
				FD_CLR(port->sock, &basemask);
				StreamClose(port->sock);
				next = DLGetSucc(curr);
				DLRemove(curr);
				free(port);
				DLFreeElem(curr);
				curr = next;
				continue;
			}
			curr = DLGetSucc(curr);
		}
		Assert(nSelected == 0);
	}
}



/*
	ConnStartup: get the startup packet from the front end (client),
	authenticate the user, and start up a backend.

	If all goes well, return *status == STATUS_OK.
	Otherwise, return *status == STATUS_ERROR and return a text string
	explaining why in the "errormsg_len" bytes at "errormsg",
*/

static void
ConnStartup(Port *port, int *status,
			char *errormsg, const int errormsg_len)
{
	MsgType		msgType;
	char		namebuf[NAMEDATALEN];
	int			pid;
	PacketBuf  *p;
	StartupInfo sp;
	char	   *tmp;

	p = &port->buf;

	sp.database[0] = '\0';
	sp.user[0] = '\0';
	sp.options[0] = '\0';
	sp.execFile[0] = '\0';
	sp.tty[0] = '\0';

	tmp = p->data;
	strncpy(sp.database, tmp, sizeof(sp.database));
	tmp += sizeof(sp.database);
	strncpy(sp.user, tmp, sizeof(sp.user));
	tmp += sizeof(sp.user);
	strncpy(sp.options, tmp, sizeof(sp.options));
	tmp += sizeof(sp.options);
	strncpy(sp.execFile, tmp, sizeof(sp.execFile));
	tmp += sizeof(sp.execFile);
	strncpy(sp.tty, tmp, sizeof(sp.tty));

	msgType = (MsgType) ntohl(port->buf.msgtype);

	StrNCpy(namebuf, sp.user, NAMEDATALEN);
	if (!namebuf[0])
	{
		strncpy(errormsg,
				"No Postgres username specified in startup packet.",
				errormsg_len);
		*status = STATUS_ERROR;
	}
	else
	{
		if (be_recvauth(msgType, port, namebuf, &sp) != STATUS_OK)
		{
			char		buffer[200 + sizeof(namebuf)];

			sprintf(buffer,
					"Failed to authenticate client as Postgres user '%s' "
					"using %s: %s",
			  namebuf, name_of_authentication_type(msgType), PQerrormsg);
			strncpy(errormsg, buffer, errormsg_len);
			*status = STATUS_ERROR;
		}
		else
		{
			if (BackendStartup(&sp, port, &pid) != STATUS_OK)
			{
				strncpy(errormsg, "Startup (fork) of backend failed.",
						errormsg_len);
				*status = STATUS_ERROR;
			}
			else
			{
				errormsg[0] = '\0';		/* just for robustness */
				*status = STATUS_OK;
			}
		}
	}
	if (*status == STATUS_ERROR)
		fprintf(stderr, "%s: ConnStartup: %s\n", progname, errormsg);
}


/*
	 send_error_reply: send a reply to the front end telling it that
	 the connection was a bust, and why.

	 "port" tells to whom and how to send the reply.  "errormsg" is
	 the string of text telling what the problem was.

	 It should be noted that we're executing a pretty messy protocol
	 here.	The postmaster does not reply when the connection is
	 successful, but rather just hands the connection off to the
	 backend and the backend waits for a query from the frontend.
	 Thus, the frontend is not expecting any reply in regards to the
	 connect request.

	 But when the connection fails, we send this reply that starts
	 with "E".	The frontend only gets this reply when it sends its
	 first query and waits for the reply.  Nobody receives that query,
	 but this reply is already in the pipe, so that's what the
	 frontend sees.

	 Note that the backend closes the socket immediately after sending
	 the reply, so to give the frontend a fighting chance to see the
	 error info, we set the socket to linger up to 3 seconds waiting
	 for the frontend to retrieve the message.	That's all the delay
	 we can afford, since we have other clients to serve and the
	 postmaster will be blocked the whole time.  Also, if there is no
	 message space in the socket for the reply (shouldn't be a
	 problem) the postmaster will block until the frontend reads the
	 reply.

*/

static void
send_error_reply(Port *port, const char *errormsg)
{
	int			rc;				/* return code from sendto */
	char	   *reply;

	/*
	 * The literal reply string we put into the socket.  This is a pointer
	 * to storage we malloc.
	 */
	const struct linger linger_parm = {true, LINGER_TIME};

	/*
	 * A parameter for setsockopt() that tells it to have close() block
	 * for a while waiting for the frontend to read its outstanding
	 * messages.
	 */

	reply = malloc(strlen(errormsg) + 10);

	sprintf(reply, "E%s", errormsg);

	rc = send(port->sock, (Addr) reply, strlen(reply) + 1, /* flags */ 0);
	if (rc < 0)
		fprintf(stderr,
				"%s: ServerLoop:\t\t"
				"Failed to send error reply to front end\n",
				progname);
	else if (rc < strlen(reply) + 1)
		fprintf(stderr,
				"%s: ServerLoop:\t\t"
				"Only partial error reply sent to front end.\n",
				progname);

	free(reply);

	/*
	 * Now we have to make sure frontend has a chance to see what we just
	 * wrote.
	 */
	rc = setsockopt(port->sock, SOL_SOCKET, SO_LINGER,
					&linger_parm, sizeof(linger_parm));
}


/*
 * ConnCreate -- create a local connection data structure
 */
static int
ConnCreate(int serverFd, int *newFdP)
{
	int			status;
	Port	   *port;


	if (!(port = (Port *) calloc(1, sizeof(Port))))
	{
		fprintf(stderr, "%s: ConnCreate: malloc failed\n",
				progname);
		ExitPostmaster(1);
	}

	if ((status = StreamConnection(serverFd, port)) != STATUS_OK)
	{
		StreamClose(port->sock);
		free(port);
	}
	else
	{
		DLAddHead(PortList, DLNewElem(port));
		*newFdP = port->sock;
	}

	return (status);
}

/*
 * reset_shared -- reset shared memory and semaphores
 */
static void
reset_shared(short port)
{
	IPCKey		key;

	key = SystemPortAddressCreateIPCKey((SystemPortAddress) port);
	CreateSharedMemoryAndSemaphores(key);
	ActiveBackends = FALSE;
}

/*
 * pmdie -- signal handler for cleaning up after a kill signal.
 */
static void
pmdie(SIGNAL_ARGS)
{
	exitpg(0);
}

/*
 * Reaper -- signal handler to cleanup after a backend (child) dies.
 */
static void
reaper(SIGNAL_ARGS)
{
/* GH: replace waitpid for !HAVE_WAITPID. Does this work ? */
#ifdef HAVE_WAITPID
	int			status;			/* backend exit status */

#else
	union wait	statusp;		/* backend exit status */

#endif
	int			pid;			/* process id of dead backend */

	if (DebugLvl)
		fprintf(stderr, "%s: reaping dead processes...\n",
				progname);
#ifdef HAVE_WAITPID
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		CleanupProc(pid, status);
		pqsignal(SIGCHLD, reaper);
	}
#else
	while ((pid = wait3(&statusp, WNOHANG, NULL)) > 0)
	{
		CleanupProc(pid, statusp.w_status);
		pqsignal(SIGCHLD, reaper);
	}
#endif
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
	Dlelem	   *prev,
			   *curr;
	Backend    *bp;
	int			sig;

	if (DebugLvl)
	{
		fprintf(stderr, "%s: CleanupProc: pid %d exited with status %d\n",
				progname, pid, exitstatus);
	}

	/*
	 * ------------------------- If a backend dies in an ugly way (i.e.
	 * exit status not 0) then we must signal all other backends to
	 * quickdie.  If exit status is zero we assume everything is hunky
	 * dory and simply remove the backend from the active backend list.
	 * -------------------------
	 */
	if (!exitstatus)
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

		ProcRemove(pid);

		return;
	}

	curr = DLGetHead(BackendList);
	while (curr)
	{
		bp = (Backend *) DLE_VAL(curr);

		/*
		 * ----------------- SIGUSR1 is the special signal that sez exit
		 * without exitpg and let the user know what's going on.
		 * ProcSemaphoreKill() cleans up the backends semaphore.  If
		 * SendStop is set (-s on command line), then we send a SIGSTOP so
		 * that we can core dumps from all backends by hand.
		 * -----------------
		 */
		sig = (SendStop) ? SIGSTOP : SIGUSR1;
		if (bp->pid != pid)
		{
			if (DebugLvl)
				fprintf(stderr, "%s: CleanupProc: sending %s to process %d\n",
						progname,
						(sig == SIGUSR1)
						? "SIGUSR1" : "SIGSTOP",
						bp->pid);
			kill(bp->pid, sig);
		}
		ProcRemove(bp->pid);

		prev = DLGetPred(curr);
		DLRemove(curr);
		free(bp);
		DLFreeElem(curr);
		if (!prev)
		{						/* removed head */
			curr = DLGetHead(BackendList);
			continue;
		}
		curr = DLGetSucc(prev);
	}

	/*
	 * ------------- Quasi_exit means run all of the on_exitpg routines
	 * but don't acutally call exit().  The on_exit list of routines to do
	 * is also truncated.
	 *
	 * Nothing up my sleeve here, ActiveBackends means that since the last
	 * time we recreated shared memory and sems another frontend has
	 * requested and received a connection and I have forked off another
	 * backend.  This prevents me from reinitializing shared stuff more
	 * than once for the set of backends that caused the failure and were
	 * killed off. ----------------
	 */
	if (ActiveBackends == TRUE && Reinit)
	{
		if (DebugLvl)
			fprintf(stderr, "%s: CleanupProc: reinitializing shared memory and semaphores\n",
					progname);
		quasi_exitpg();
		reset_shared(PostPortName);
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
BackendStartup(StartupInfo *packet,		/* client's startup packet */
			   Port *port,
			   int *pidPtr)
{
	Backend    *bn;				/* for backend cleanup */
	int			pid,
				i;
	static char envEntry[4][2 * ARGV_SIZE];

	for (i = 0; i < 4; ++i)
	{
		MemSet(envEntry[i], 0, 2 * ARGV_SIZE);
	}

	/*
	 * Set up the necessary environment variables for the backend This
	 * should really be some sort of message....
	 */
	sprintf(envEntry[0], "POSTPORT=%d", PostPortName);
	putenv(envEntry[0]);
	sprintf(envEntry[1], "POSTID=%d", NextBackendId);
	putenv(envEntry[1]);
	sprintf(envEntry[2], "PG_USER=%s", packet->user);
	putenv(envEntry[2]);
	if (!getenv("PGDATA"))
	{
		sprintf(envEntry[3], "PGDATA=%s", DataDir);
		putenv(envEntry[3]);
	}
	if (DebugLvl > 2)
	{
		char	  **p;
		extern char **environ;

		fprintf(stderr, "%s: BackendStartup: environ dump:\n",
				progname);
		fprintf(stderr, "-----------------------------------------\n");
		for (p = environ; *p; ++p)
			fprintf(stderr, "\t%s\n", *p);
		fprintf(stderr, "-----------------------------------------\n");
	}

	if ((pid = FORK()) == 0)
	{							/* child */
		if (DoExec(packet, port->sock))
			fprintf(stderr, "%s child[%d]: BackendStartup: execv failed\n",
					progname, pid);
		/* use _exit to keep from double-flushing stdio */
		_exit(1);
	}

	/* in parent */
	if (pid < 0)
	{
		fprintf(stderr, "%s: BackendStartup: fork failed\n",
				progname);
		return (STATUS_ERROR);
	}

	if (DebugLvl)
		fprintf(stderr, "%s: BackendStartup: pid %d user %s db %s socket %d\n",
				progname, pid, packet->user,
		 (packet->database[0] == '\0' ? packet->user : packet->database),
				port->sock);

	/* adjust backend counter */
	/* XXX Don't know why this is done, but for now backend needs it */
	NextBackendId -= 1;

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
	DLAddHead(BackendList, DLNewElem(bn));

	if (MultiplexedBackends)
		MultiplexedBackendPort++;

	*pidPtr = pid;

	return (STATUS_OK);
}

/*
 * split_opts -- destructively load a string into an argv array
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
	int			i = *argcp;

	while (s && *s)
	{
		while (isspace(*s))
			++s;
		if (*s)
			argv[i++] = s;
		while (*s && !isspace(*s))
			++s;
		if (isspace(*s))
			*s++ = '\0';
	}
	*argcp = i;
}

/*
 * DoExec -- set up the argument list and perform an execv system call
 *
 * Tries fairly hard not to dork with anything that isn't automatically
 * allocated so we don't do anything weird to the postmaster when it gets
 * its thread back.  (This is vfork() we're talking about.  If we're using
 * fork() because we don't have vfork(), then we don't really care.)
 *
 * returns:
 *		Shouldn't return at all.
 *		If execv() fails, return status.
 */
static int
DoExec(StartupInfo *packet, int portFd)
{
	char		execbuf[MAXPATHLEN];
	char		portbuf[ARGV_SIZE];
	char		mbbuf[ARGV_SIZE];
	char		debugbuf[ARGV_SIZE];
	char		ttybuf[ARGV_SIZE + 1];
	char		argbuf[(2 * ARGV_SIZE) + 1];

	/*
	 * each argument takes at least three chars, so we can't have more
	 * than ARGV_SIZE arguments in (2 * ARGV_SIZE) chars (i.e.,
	 * packet->options plus ExtraOptions)...
	 */
	char	   *av[ARGV_SIZE];
	char		dbbuf[ARGV_SIZE + 1];
	int			ac = 0;
	int			i;

	strncpy(execbuf, Execfile, MAXPATHLEN - 1);
	av[ac++] = execbuf;

	/* Tell the backend it is being called from the postmaster */
	av[ac++] = "-p";

	/*
	 * Pass the requested debugging level along to the backend.  We
	 * decrement by one; level one debugging in the postmaster traces
	 * postmaster connection activity, and levels two and higher are
	 * passed along to the backend.  This allows us to watch only the
	 * postmaster or the postmaster and the backend.
	 */

	if (DebugLvl > 1)
	{
		sprintf(debugbuf, "-d%d", DebugLvl);
		av[ac++] = debugbuf;
	}
	else
		av[ac++] = "-Q";

	/* Pass the requested debugging output file */
	if (packet->tty[0])
	{
		strncpy(ttybuf, packet->tty, ARGV_SIZE);
		av[ac++] = "-o";
		av[ac++] = ttybuf;
	}

	/* tell the multiplexed backend to start on a certain port */
	if (MultiplexedBackends)
	{
		sprintf(mbbuf, "-m %d", MultiplexedBackendPort);
		av[ac++] = mbbuf;
	}
	/* Tell the backend the descriptor of the fe/be socket */
	sprintf(portbuf, "-P%d", portFd);
	av[ac++] = portbuf;

	StrNCpy(argbuf, packet->options, ARGV_SIZE);
	strncat(argbuf, ExtraOptions, ARGV_SIZE);
	argbuf[(2 * ARGV_SIZE)] = '\0';
	split_opts(av, &ac, argbuf);

	if (packet->database[0])
		StrNCpy(dbbuf, packet->database, ARGV_SIZE);
	else
		StrNCpy(dbbuf, packet->user, NAMEDATALEN);
	av[ac++] = dbbuf;

	av[ac] = (char *) NULL;

	if (DebugLvl > 1)
	{
		fprintf(stderr, "%s child[%ld]: execv(",
				progname, (long) getpid());
		for (i = 0; i < ac; ++i)
			fprintf(stderr, "%s, ", av[i]);
		fprintf(stderr, ")\n");
	}

	return (execv(av[0], av));
}

/*
 * ExitPostmaster -- cleanup
 */
static void
ExitPostmaster(int status)
{
	/* should cleanup shared memory and kill all backends */

	/*
	 * Not sure of the semantics here.	When the Postmaster dies, should
	 * the backends all be killed? probably not.
	 */
	if (ServerSock != INVALID_SOCK)
		close(ServerSock);
	exitpg(status);
}

static void
dumpstatus(SIGNAL_ARGS)
{
	Dlelem	   *curr = DLGetHead(PortList);

	while (curr)
	{
		Port	   *port = DLE_VAL(curr);

		fprintf(stderr, "%s: dumpstatus:\n", progname);
		fprintf(stderr, "\tsock %d: nBytes=%d, laddr=0x%lx, raddr=0x%lx\n",
				port->sock, port->nBytes,
				(long int) port->laddr.sin_addr.s_addr,
				(long int) port->raddr.sin_addr.s_addr);
		curr = DLGetSucc(curr);
	}
}
