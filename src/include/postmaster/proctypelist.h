/*-------------------------------------------------------------------------
 *
 * proctypelist.h
 *
 * The list of process types is kept on its own source file for use by
 * automatic tools.  The exact representation of a process type is
 * determined by the PG_PROCTYPE macro, which is not defined in this
 * file; it can be defined by the caller for special purposes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/postmaster/proctypelist.h
 *
 *-------------------------------------------------------------------------
 */

/* there is deliberately not an #ifndef PROCTYPELIST_H here */

/*
 * WAL senders start their life as regular backend processes, and change their
 * type after authenticating the client for replication.  We list it here for
 * PostmasterChildName() but cannot launch them directly.
 */

/*
 * List of process types (symbol, description, Main function, shmem_attach)
 * entries.
 */


/* bktype, description, main_func, shmem_attach */
PG_PROCTYPE(B_ARCHIVER, gettext_noop("archiver"), PgArchiverMain, true)
PG_PROCTYPE(B_AUTOVAC_LAUNCHER, gettext_noop("autovacuum launcher"), AutoVacLauncherMain, true)
PG_PROCTYPE(B_AUTOVAC_WORKER, gettext_noop("autovacuum worker"), AutoVacWorkerMain, true)
PG_PROCTYPE(B_BACKEND, gettext_noop("client backend"), BackendMain, true)
PG_PROCTYPE(B_BG_WORKER, gettext_noop("background worker"), BackgroundWorkerMain, true)
PG_PROCTYPE(B_BG_WRITER, gettext_noop("background writer"), BackgroundWriterMain, true)
PG_PROCTYPE(B_CHECKPOINTER, gettext_noop("checkpointer"), CheckpointerMain, true)
PG_PROCTYPE(B_DEAD_END_BACKEND, gettext_noop("dead-end client backend"), BackendMain, true)
PG_PROCTYPE(B_INVALID, gettext_noop("unrecognized"), NULL, false)
PG_PROCTYPE(B_IO_WORKER, gettext_noop("io worker"), IoWorkerMain, true)
PG_PROCTYPE(B_LOGGER, gettext_noop("syslogger"), SysLoggerMain, false)
PG_PROCTYPE(B_SLOTSYNC_WORKER, gettext_noop("slotsync worker"), ReplSlotSyncWorkerMain, true)
PG_PROCTYPE(B_STANDALONE_BACKEND, gettext_noop("standalone backend"), NULL, false)
PG_PROCTYPE(B_STARTUP, gettext_noop("startup"), StartupProcessMain, true)
PG_PROCTYPE(B_WAL_RECEIVER, gettext_noop("walreceiver"), WalReceiverMain, true)
PG_PROCTYPE(B_WAL_SENDER, gettext_noop("walsender"), NULL, true)
PG_PROCTYPE(B_WAL_SUMMARIZER, gettext_noop("walsummarizer"), WalSummarizerMain, true)
PG_PROCTYPE(B_WAL_WRITER, gettext_noop("walwriter"), WalWriterMain, true)
