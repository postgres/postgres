/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  Exports from postmaster/postmaster.c.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

#include "lib/ilist.h"
#include "miscadmin.h"

/*
 * A struct representing an active postmaster child process.  This is used
 * mainly to keep track of how many children we have and send them appropriate
 * signals when necessary.  All postmaster child processes are assigned a
 * PMChild entry.  That includes "normal" client sessions, but also autovacuum
 * workers, walsenders, background workers, and aux processes.  (Note that at
 * the time of launch, walsenders are labeled B_BACKEND; we relabel them to
 * B_WAL_SENDER upon noticing they've changed their PMChildFlags entry.  Hence
 * that check must be done before any operation that needs to distinguish
 * walsenders from normal backends.)
 *
 * "dead-end" children are also allocated a PMChild entry: these are children
 * launched just for the purpose of sending a friendly rejection message to a
 * would-be client.  We must track them because they are attached to shared
 * memory, but we know they will never become live backends.
 *
 * child_slot is an identifier that is unique across all running child
 * processes.  It is used as an index into the PMChildFlags array.  dead-end
 * children are not assigned a child_slot and have child_slot == 0 (valid
 * child_slot ids start from 1).
 */
typedef struct
{
	pid_t		pid;			/* process id of backend */
	int			child_slot;		/* PMChildSlot for this backend, if any */
	BackendType bkend_type;		/* child process flavor, see above */
	struct RegisteredBgWorker *rw;	/* bgworker info, if this is a bgworker */
	bool		bgworker_notify;	/* gets bgworker start/stop notifications */
	dlist_node	elem;			/* list link in ActiveChildList */
} PMChild;

#ifdef EXEC_BACKEND
extern int	num_pmchild_slots;
#endif

/* GUC options */
extern PGDLLIMPORT bool EnableSSL;
extern PGDLLIMPORT int SuperuserReservedConnections;
extern PGDLLIMPORT int ReservedConnections;
extern PGDLLIMPORT int PostPortNumber;
extern PGDLLIMPORT int Unix_socket_permissions;
extern PGDLLIMPORT char *Unix_socket_group;
extern PGDLLIMPORT char *Unix_socket_directories;
extern PGDLLIMPORT char *ListenAddresses;
extern PGDLLIMPORT bool ClientAuthInProgress;
extern PGDLLIMPORT int PreAuthDelay;
extern PGDLLIMPORT int AuthenticationTimeout;
extern PGDLLIMPORT bool Log_connections;
extern PGDLLIMPORT bool log_hostname;
extern PGDLLIMPORT bool enable_bonjour;
extern PGDLLIMPORT char *bonjour_name;
extern PGDLLIMPORT bool restart_after_crash;
extern PGDLLIMPORT bool remove_temp_files_after_crash;
extern PGDLLIMPORT bool send_abort_for_crash;
extern PGDLLIMPORT bool send_abort_for_kill;

#ifdef WIN32
extern PGDLLIMPORT HANDLE PostmasterHandle;
#else
extern PGDLLIMPORT int postmaster_alive_fds[2];

/*
 * Constants that represent which of postmaster_alive_fds is held by
 * postmaster, and which is used in children to check for postmaster death.
 */
#define POSTMASTER_FD_WATCH		0	/* used in children to check for
									 * postmaster death */
#define POSTMASTER_FD_OWN		1	/* kept open by postmaster only */
#endif

extern PGDLLIMPORT const char *progname;

extern PGDLLIMPORT bool redirection_done;
extern PGDLLIMPORT bool LoadedSSL;

extern void PostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
extern void ClosePostmasterPorts(bool am_syslogger);
extern void InitProcessGlobals(void);

extern int	MaxLivePostmasterChildren(void);

extern bool PostmasterMarkPIDForWorkerNotify(int);

#ifdef WIN32
extern void pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId);
#endif

/* defined in globals.c */
extern PGDLLIMPORT struct ClientSocket *MyClientSocket;

/* prototypes for functions in launch_backend.c */
extern pid_t postmaster_child_launch(BackendType child_type,
									 int child_slot,
									 const void *startup_data,
									 size_t startup_data_len,
									 struct ClientSocket *client_sock);
const char *PostmasterChildName(BackendType child_type);
#ifdef EXEC_BACKEND
extern void SubPostmasterMain(int argc, char *argv[]) pg_attribute_noreturn();
#endif

/* defined in pmchild.c */
extern dlist_head ActiveChildList;

extern void InitPostmasterChildSlots(void);
extern PMChild *AssignPostmasterChildSlot(BackendType btype);
extern PMChild *AllocDeadEndChild(void);
extern bool ReleasePostmasterChildSlot(PMChild *pmchild);
extern PMChild *FindPostmasterChildByPid(int pid);

/*
 * These values correspond to the special must-be-first options for dispatching
 * to various subprograms.  parse_dispatch_option() can be used to convert an
 * option name to one of these values.
 */
typedef enum DispatchOption
{
	DISPATCH_CHECK,
	DISPATCH_BOOT,
	DISPATCH_FORKCHILD,
	DISPATCH_DESCRIBE_CONFIG,
	DISPATCH_SINGLE,
	DISPATCH_POSTMASTER,		/* must be last */
} DispatchOption;

extern DispatchOption parse_dispatch_option(const char *name);

#endif							/* _POSTMASTER_H */
