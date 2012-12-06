/*--------------------------------------------------------------------
 * bgworker.h
 * 		POSTGRES pluggable background workers interface
 *
 * A background worker is a process able to run arbitrary, user-supplied code,
 * including normal transactions.
 *
 * Any external module loaded via shared_preload_libraries can register a
 * worker.  Then, at the appropriate time, the worker process is forked from
 * the postmaster and runs the user-supplied "main" function.  This code may
 * connect to a database and run transactions.  Once started, it stays active
 * until shutdown or crash.  The process should sleep during periods of
 * inactivity.
 *
 * If the fork() call fails in the postmaster, it will try again later.  Note
 * that the failure can only be transient (fork failure due to high load,
 * memory pressure, too many processes, etc); more permanent problems, like
 * failure to connect to a database, are detected later in the worker and dealt
 * with just by having the worker exit normally.  Postmaster will launch a new
 * worker again later.
 *
 * Note that there might be more than one worker in a database concurrently,
 * and the same module may request more than one worker running the same (or
 * different) code.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * 		src/include/postmaster/bgworker.h
 *--------------------------------------------------------------------
 */
#ifndef BGWORKER_H
#define BGWORKER_H

/*---------------------------------------------------------------------
 * External module API.
 *---------------------------------------------------------------------
 */

/*
 * Pass this flag to have your worker be able to connect to shared memory.
 */
#define BGWORKER_SHMEM_ACCESS						0x0001

/*
 * This flag means the bgworker requires a database connection.  The connection
 * is not established automatically; the worker must establish it later.
 * It requires that BGWORKER_SHMEM_ACCESS was passed too.
 */
#define BGWORKER_BACKEND_DATABASE_CONNECTION		0x0002


typedef void (*bgworker_main_type)(void *main_arg);
typedef void (*bgworker_sighdlr_type)(SIGNAL_ARGS);

/*
 * Points in time at which a bgworker can request to be started
 */
typedef enum
{
	BgWorkerStart_PostmasterStart,
	BgWorkerStart_ConsistentState,
	BgWorkerStart_RecoveryFinished
} BgWorkerStartTime;

#define BGW_DEFAULT_RESTART_INTERVAL	60
#define BGW_NEVER_RESTART				-1

typedef struct BackgroundWorker
{
	char	   *bgw_name;
	int         bgw_flags;
	BgWorkerStartTime bgw_start_time;
	int			bgw_restart_time;		/* in seconds, or BGW_NEVER_RESTART */
	bgworker_main_type	bgw_main;
	void	   *bgw_main_arg;
	bgworker_sighdlr_type bgw_sighup;
	bgworker_sighdlr_type bgw_sigterm;
} BackgroundWorker;

/* Register a new bgworker */
extern void RegisterBackgroundWorker(BackgroundWorker *worker);

/* This is valid in a running worker */
extern BackgroundWorker *MyBgworkerEntry;

/*
 * Connect to the specified database, as the specified user.  Only a worker
 * that passed BGWORKER_BACKEND_DATABASE_CONNECTION during registration may
 * call this.
 *
 * If username is NULL, bootstrapping superuser is used.
 * If dbname is NULL, connection is made to no specific database;
 * only shared catalogs can be accessed.
 */
extern void BackgroundWorkerInitializeConnection(char *dbname, char *username);

/* Block/unblock signals in a background worker process */
extern void BackgroundWorkerBlockSignals(void);
extern void BackgroundWorkerUnblockSignals(void);

#endif /* BGWORKER_H */
