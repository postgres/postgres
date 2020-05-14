/*-------------------------------------------------------------------------
 *
 * walreceiver.h
 *	  Exports from replication/walreceiverfuncs.c.
 *
 * Portions Copyright (c) 2010-2020, PostgreSQL Global Development Group
 *
 * src/include/replication/walreceiver.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALRECEIVER_H
#define _WALRECEIVER_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "getaddrinfo.h"		/* for NI_MAXHOST */
#include "pgtime.h"
#include "port/atomics.h"
#include "replication/logicalproto.h"
#include "replication/walsender.h"
#include "storage/latch.h"
#include "storage/spin.h"
#include "utils/tuplestore.h"

/* user-settable parameters */
extern int	wal_receiver_status_interval;
extern int	wal_receiver_timeout;
extern bool hot_standby_feedback;

/*
 * MAXCONNINFO: maximum size of a connection string.
 *
 * XXX: Should this move to pg_config_manual.h?
 */
#define MAXCONNINFO		1024

/* Can we allow the standby to accept replication connection from another standby? */
#define AllowCascadeReplication() (EnableHotStandby && max_wal_senders > 0)

/*
 * Values for WalRcv->walRcvState.
 */
typedef enum
{
	WALRCV_STOPPED,				/* stopped and mustn't start up again */
	WALRCV_STARTING,			/* launched, but the process hasn't
								 * initialized yet */
	WALRCV_STREAMING,			/* walreceiver is streaming */
	WALRCV_WAITING,				/* stopped streaming, waiting for orders */
	WALRCV_RESTARTING,			/* asked to restart streaming */
	WALRCV_STOPPING				/* requested to stop, but still running */
} WalRcvState;

/* Shared memory area for management of walreceiver process */
typedef struct
{
	/*
	 * PID of currently active walreceiver process, its current state and
	 * start time (actually, the time at which it was requested to be
	 * started).
	 */
	pid_t		pid;
	WalRcvState walRcvState;
	pg_time_t	startTime;

	/*
	 * receiveStart and receiveStartTLI indicate the first byte position and
	 * timeline that will be received. When startup process starts the
	 * walreceiver, it sets these to the point where it wants the streaming to
	 * begin.
	 */
	XLogRecPtr	receiveStart;
	TimeLineID	receiveStartTLI;

	/*
	 * flushedUpto-1 is the last byte position that has already been received,
	 * and receivedTLI is the timeline it came from.  At the first startup of
	 * walreceiver, these are set to receiveStart and receiveStartTLI. After
	 * that, walreceiver updates these whenever it flushes the received WAL to
	 * disk.
	 */
	XLogRecPtr	flushedUpto;
	TimeLineID	receivedTLI;

	/*
	 * latestChunkStart is the starting byte position of the current "batch"
	 * of received WAL.  It's actually the same as the previous value of
	 * flushedUpto before the last flush to disk.  Startup process can use
	 * this to detect whether it's keeping up or not.
	 */
	XLogRecPtr	latestChunkStart;

	/*
	 * Time of send and receive of any message received.
	 */
	TimestampTz lastMsgSendTime;
	TimestampTz lastMsgReceiptTime;

	/*
	 * Latest reported end of WAL on the sender
	 */
	XLogRecPtr	latestWalEnd;
	TimestampTz latestWalEndTime;

	/*
	 * connection string; initially set to connect to the primary, and later
	 * clobbered to hide security-sensitive fields.
	 */
	char		conninfo[MAXCONNINFO];

	/*
	 * Host name (this can be a host name, an IP address, or a directory path)
	 * and port number of the active replication connection.
	 */
	char		sender_host[NI_MAXHOST];
	int			sender_port;

	/*
	 * replication slot name; is also used for walreceiver to connect with the
	 * primary
	 */
	char		slotname[NAMEDATALEN];

	/*
	 * If it's a temporary replication slot, it needs to be recreated when
	 * connecting.
	 */
	bool		is_temp_slot;

	/* set true once conninfo is ready to display (obfuscated pwds etc) */
	bool		ready_to_display;

	/*
	 * Latch used by startup process to wake up walreceiver after telling it
	 * where to start streaming (after setting receiveStart and
	 * receiveStartTLI), and also to tell it to send apply feedback to the
	 * primary whenever specially marked commit records are applied. This is
	 * normally mapped to procLatch when walreceiver is running.
	 */
	Latch	   *latch;

	slock_t		mutex;			/* locks shared variables shown above */

	/*
	 * Like flushedUpto, but advanced after writing and before flushing,
	 * without the need to acquire the spin lock.  Data can be read by another
	 * process up to this point, but shouldn't be used for data integrity
	 * purposes.
	 */
	pg_atomic_uint64 writtenUpto;

	/*
	 * force walreceiver reply?  This doesn't need to be locked; memory
	 * barriers for ordering are sufficient.  But we do need atomic fetch and
	 * store semantics, so use sig_atomic_t.
	 */
	sig_atomic_t force_reply;	/* used as a bool */
} WalRcvData;

extern WalRcvData *WalRcv;

typedef struct
{
	bool		logical;		/* True if this is logical replication stream,
								 * false if physical stream.  */
	char	   *slotname;		/* Name of the replication slot or NULL. */
	XLogRecPtr	startpoint;		/* LSN of starting point. */

	union
	{
		struct
		{
			TimeLineID	startpointTLI;	/* Starting timeline */
		}			physical;
		struct
		{
			uint32		proto_version;	/* Logical protocol version */
			List	   *publication_names;	/* String list of publications */
		}			logical;
	}			proto;
} WalRcvStreamOptions;

struct WalReceiverConn;
typedef struct WalReceiverConn WalReceiverConn;

/*
 * Status of walreceiver query execution.
 *
 * We only define statuses that are currently used.
 */
typedef enum
{
	WALRCV_ERROR,				/* There was error when executing the query. */
	WALRCV_OK_COMMAND,			/* Query executed utility or replication
								 * command. */
	WALRCV_OK_TUPLES,			/* Query returned tuples. */
	WALRCV_OK_COPY_IN,			/* Query started COPY FROM. */
	WALRCV_OK_COPY_OUT,			/* Query started COPY TO. */
	WALRCV_OK_COPY_BOTH			/* Query started COPY BOTH replication
								 * protocol. */
} WalRcvExecStatus;

/*
 * Return value for walrcv_exec, returns the status of the execution and
 * tuples if any.
 */
typedef struct WalRcvExecResult
{
	WalRcvExecStatus status;
	char	   *err;
	Tuplestorestate *tuplestore;
	TupleDesc	tupledesc;
} WalRcvExecResult;

/* libpqwalreceiver hooks */
typedef WalReceiverConn *(*walrcv_connect_fn) (const char *conninfo, bool logical,
											   const char *appname,
											   char **err);
typedef void (*walrcv_check_conninfo_fn) (const char *conninfo);
typedef char *(*walrcv_get_conninfo_fn) (WalReceiverConn *conn);
typedef void (*walrcv_get_senderinfo_fn) (WalReceiverConn *conn,
										  char **sender_host,
										  int *sender_port);
typedef char *(*walrcv_identify_system_fn) (WalReceiverConn *conn,
											TimeLineID *primary_tli);
typedef int (*walrcv_server_version_fn) (WalReceiverConn *conn);
typedef void (*walrcv_readtimelinehistoryfile_fn) (WalReceiverConn *conn,
												   TimeLineID tli,
												   char **filename,
												   char **content, int *size);
typedef bool (*walrcv_startstreaming_fn) (WalReceiverConn *conn,
										  const WalRcvStreamOptions *options);
typedef void (*walrcv_endstreaming_fn) (WalReceiverConn *conn,
										TimeLineID *next_tli);
typedef int (*walrcv_receive_fn) (WalReceiverConn *conn, char **buffer,
								  pgsocket *wait_fd);
typedef void (*walrcv_send_fn) (WalReceiverConn *conn, const char *buffer,
								int nbytes);
typedef char *(*walrcv_create_slot_fn) (WalReceiverConn *conn,
										const char *slotname, bool temporary,
										CRSSnapshotAction snapshot_action,
										XLogRecPtr *lsn);
typedef pid_t (*walrcv_get_backend_pid_fn) (WalReceiverConn *conn);
typedef WalRcvExecResult *(*walrcv_exec_fn) (WalReceiverConn *conn,
											 const char *query,
											 const int nRetTypes,
											 const Oid *retTypes);
typedef void (*walrcv_disconnect_fn) (WalReceiverConn *conn);

typedef struct WalReceiverFunctionsType
{
	walrcv_connect_fn walrcv_connect;
	walrcv_check_conninfo_fn walrcv_check_conninfo;
	walrcv_get_conninfo_fn walrcv_get_conninfo;
	walrcv_get_senderinfo_fn walrcv_get_senderinfo;
	walrcv_identify_system_fn walrcv_identify_system;
	walrcv_server_version_fn walrcv_server_version;
	walrcv_readtimelinehistoryfile_fn walrcv_readtimelinehistoryfile;
	walrcv_startstreaming_fn walrcv_startstreaming;
	walrcv_endstreaming_fn walrcv_endstreaming;
	walrcv_receive_fn walrcv_receive;
	walrcv_send_fn walrcv_send;
	walrcv_create_slot_fn walrcv_create_slot;
	walrcv_get_backend_pid_fn walrcv_get_backend_pid;
	walrcv_exec_fn walrcv_exec;
	walrcv_disconnect_fn walrcv_disconnect;
} WalReceiverFunctionsType;

extern PGDLLIMPORT WalReceiverFunctionsType *WalReceiverFunctions;

#define walrcv_connect(conninfo, logical, appname, err) \
	WalReceiverFunctions->walrcv_connect(conninfo, logical, appname, err)
#define walrcv_check_conninfo(conninfo) \
	WalReceiverFunctions->walrcv_check_conninfo(conninfo)
#define walrcv_get_conninfo(conn) \
	WalReceiverFunctions->walrcv_get_conninfo(conn)
#define walrcv_get_senderinfo(conn, sender_host, sender_port) \
	WalReceiverFunctions->walrcv_get_senderinfo(conn, sender_host, sender_port)
#define walrcv_identify_system(conn, primary_tli) \
	WalReceiverFunctions->walrcv_identify_system(conn, primary_tli)
#define walrcv_server_version(conn) \
	WalReceiverFunctions->walrcv_server_version(conn)
#define walrcv_readtimelinehistoryfile(conn, tli, filename, content, size) \
	WalReceiverFunctions->walrcv_readtimelinehistoryfile(conn, tli, filename, content, size)
#define walrcv_startstreaming(conn, options) \
	WalReceiverFunctions->walrcv_startstreaming(conn, options)
#define walrcv_endstreaming(conn, next_tli) \
	WalReceiverFunctions->walrcv_endstreaming(conn, next_tli)
#define walrcv_receive(conn, buffer, wait_fd) \
	WalReceiverFunctions->walrcv_receive(conn, buffer, wait_fd)
#define walrcv_send(conn, buffer, nbytes) \
	WalReceiverFunctions->walrcv_send(conn, buffer, nbytes)
#define walrcv_create_slot(conn, slotname, temporary, snapshot_action, lsn) \
	WalReceiverFunctions->walrcv_create_slot(conn, slotname, temporary, snapshot_action, lsn)
#define walrcv_get_backend_pid(conn) \
	WalReceiverFunctions->walrcv_get_backend_pid(conn)
#define walrcv_exec(conn, exec, nRetTypes, retTypes) \
	WalReceiverFunctions->walrcv_exec(conn, exec, nRetTypes, retTypes)
#define walrcv_disconnect(conn) \
	WalReceiverFunctions->walrcv_disconnect(conn)

static inline void
walrcv_clear_result(WalRcvExecResult *walres)
{
	if (!walres)
		return;

	if (walres->err)
		pfree(walres->err);

	if (walres->tuplestore)
		tuplestore_end(walres->tuplestore);

	if (walres->tupledesc)
		FreeTupleDesc(walres->tupledesc);

	pfree(walres);
}

/* prototypes for functions in walreceiver.c */
extern void WalReceiverMain(void) pg_attribute_noreturn();
extern void ProcessWalRcvInterrupts(void);

/* prototypes for functions in walreceiverfuncs.c */
extern Size WalRcvShmemSize(void);
extern void WalRcvShmemInit(void);
extern void ShutdownWalRcv(void);
extern bool WalRcvStreaming(void);
extern bool WalRcvRunning(void);
extern void RequestXLogStreaming(TimeLineID tli, XLogRecPtr recptr,
								 const char *conninfo, const char *slotname,
								 bool create_temp_slot);
extern XLogRecPtr GetWalRcvFlushRecPtr(XLogRecPtr *latestChunkStart, TimeLineID *receiveTLI);
extern XLogRecPtr GetWalRcvWriteRecPtr(void);
extern int	GetReplicationApplyDelay(void);
extern int	GetReplicationTransferLatency(void);
extern void WalRcvForceReply(void);

#endif							/* _WALRECEIVER_H */
