#ifndef __SAFEKEEPER_H__
#define __SAFEKEEPER_H__

#include "postgres_fe.h"
#include "access/xlog_internal.h"
#include "access/transam.h"
#include "libpq-int.h"
#include "utils/uuid.h"

#define SK_MAGIC               0xCafeCeefu
#define SK_FORMAT_VERSION      1
#define SK_PROTOCOL_VERSION    1
#define UNKNOWN_SERVER_VERSION 0

#define MAX_SAFEKEEPERS        32
#define MAX_SEND_SIZE         (XLOG_BLCKSZ * 16)
#define XLOG_HDR_SIZE         (1+8*3)  /* 'w' + startPos + walEnd + timestamp */
#define XLOG_HDR_START_POS    1        /* offset of start position in wal sender message header */
#define XLOG_HDR_END_POS      (1+8)    /* offset of end position in wal sender message header */
#define KEEPALIVE_RR_OFFS     17       /* offset of reply requested field in keep alive request */
#define LIBPQ_HDR_SIZE        5        /* 1 byte with message type + 4 bytes length */
#define REPLICA_FEEDBACK_SIZE 64       /* size of replica's feedback */
#define HS_FEEDBACK_SIZE      25       /* hot standby feedback size */
#define LIBPQ_MSG_SIZE_OFFS   1        /* offset of message size inside libpq header */
#define LIBPQ_DATA_SIZE(sz)   ((sz)-4) /* size of libpq message includes 4-bytes size field */
#define END_OF_STREAM         InvalidXLogRecPtr

struct WalMessage;
typedef struct WalMessage WalMessage;

/* Safekeeper_proxy states */
typedef enum
{
	SS_OFFLINE,
	SS_CONNECTING,
	SS_HANDSHAKE,
	SS_VOTING,
	SS_WAIT_VERDICT,
	SS_IDLE,
	SS_SEND_WAL,
	SS_RECV_FEEDBACK
} SafeKeeperState;

/*
 * Unique node identifier used by Paxos
 */
typedef struct NodeId
{
	uint64    term;
	pg_uuid_t uuid;
} NodeId;

/*
 * Information about Postgres server broadcasted by safekeeper_proxy to safekeeper
 */
typedef struct ServerInfo
{
	uint32     protocolVersion;   /* proxy-safekeeper protocol version */
	uint32     pgVersion;         /* Postgres server version */
	NodeId     nodeId;
	uint64     systemId;          /* Postgres system identifier */
	uint8	   ztimelineid[16];   /* Zenith timeline id */
	XLogRecPtr walEnd;
    TimeLineID timeline;
	int        walSegSize;
} ServerInfo;

/*
 * Vote request sent from proxy to safekeepers
 */
typedef struct RequestVote
{
	NodeId     nodeId;
	XLogRecPtr VCL;   /* volume commit LSN */
	uint64     epoch; /* new epoch when safekeeper reaches VCL */
} RequestVote;

/*
 * Information of about storage node
 */
typedef struct SafeKeeperInfo
{
	uint32     magic;             /* magic for verifying content the control file */
	uint32     formatVersion;     /* safekeeper format version */
	uint64     epoch;             /* safekeeper's epoch */
	ServerInfo server;
	XLogRecPtr commitLsn;         /* part of WAL acknowledged by quorum */
	XLogRecPtr flushLsn;          /* locally flushed part of WAL */
	XLogRecPtr restartLsn;        /* minimal LSN which may be needed for recovery of some safekeeper: min(commitLsn) for all safekeepers */
} SafeKeeperInfo;

/*
 * Hot standby feedback received from replica
 */
typedef struct HotStandbyFeedback
{
	TimestampTz       ts;
	FullTransactionId xmin;
	FullTransactionId catalog_xmin;
} HotStandbyFeedback;

/*
 * WAL sender context
 */
typedef struct WalSender
{
	struct WalSender*  next; /* L2-List entry */
	struct WalSender*  prev;
	pthread_t          thread;
	pgsocket           sock;
	char const*        basedir;
	int                startupPacketLength;
	int                walSegSize;
	uint64             systemId;
	HotStandbyFeedback hsFeedback;
	XLogRecPtr         stopLsn;
} WalSender;


/*
 * Request with WAL message sent from proxy to safekeeper.
 */
typedef struct SafekeeperRequest
{
	NodeId     senderId;    /* Sender's node identifier (looks like we do not need it for TCP streaming connection) */
	XLogRecPtr beginLsn;    /* start position of message in WAL */
	XLogRecPtr endLsn;      /* end position of message in WAL */
	XLogRecPtr restartLsn;  /* restart LSN position  (minimal LSN which may be needed by proxy to perform recovery) */
	XLogRecPtr commitLsn;   /* LSN committed by quorum of safekeepers */
} SafekeeperRequest;

/*
 * All copy data message ('w') are linked in L1 send list and asynchronously sent to receivers.
 * When message is sent to all receivers, it is removed from send list.
 */
struct WalMessage
{
	WalMessage* next;      /* L1 list of messages */
	uint32 size;           /* message size */
	uint32 ackMask;        /* mask of receivers acknowledged receiving of this message */
	SafekeeperRequest req; /* request to safekeeper (message header) */
};

/*
 * Report safekeeper state to proxy
 */
typedef struct SafekeeperResponse
{
	uint64     epoch;
	XLogRecPtr flushLsn;
	HotStandbyFeedback hs;
} SafekeeperResponse;


/*
 * Descriptor of safekeeper
 */
typedef struct Safekeeper
{
    char const* host;
    char const* port;
	pgsocket    sock;     /* socket descriptor */
	WalMessage* currMsg;  /* message been send to the receiver */
	int         asyncOffs;/* offset for asynchronus read/write operations */
	SafeKeeperState state;/* safekeeper state machine state */
    SafeKeeperInfo  info; /* safekeeper info */
	SafekeeperResponse feedback; /* feedback to master */
} Safekeeper;


int        CompareNodeId(NodeId* id1, NodeId* id2);
pgsocket   CreateSocket(char const* host, char const* port, int n_peers);
pgsocket   ConnectSocketAsync(char const* host, char const* port, bool* established);
bool       WriteSocket(pgsocket sock, void const* buf, size_t size);
bool       ReadSocket(pgsocket sock, void* buf, size_t size);
bool       ReadSocketNowait(pgsocket sock, void* buf, size_t size);
ssize_t    ReadSocketAsync(pgsocket sock, void* buf, size_t size);
ssize_t    WriteSocketAsync(pgsocket sock, void const* buf, size_t size);
bool       SaveData(int file, void const* data, size_t size, bool do_sync);
int        CompareLsn(const void *a, const void *b);
void       StartWalSender(pgsocket sock, char const* basedir, int startupPacketLength, int walSegSize, uint64 systemId);
void       StopWalSenders(void);
void       NotifyWalSenders(XLogRecPtr lsn);
void       fe_sendint32(int32 i, char *buf);
int32      fe_recvint32(char *buf);
void       fe_sendint16(int16 i, char *buf);
int16      fe_recvint16(char *buf);
XLogRecPtr FindEndOfWAL(TimeLineID *tli, bool precise);
void       CollectHotStanbyFeedbacks(HotStandbyFeedback* hs);
PGconn*    ConnectSafekeeper(char const* host, char const* port);

#endif
