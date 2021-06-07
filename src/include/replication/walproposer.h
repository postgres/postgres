#ifndef __WALKEEPER_H__
#define __WALKEEPER_H__

#include "postgres.h"
#include "access/xlog_internal.h"
#include "access/transam.h"
#include "nodes/replnodes.h"
#include "utils/uuid.h"

#define SK_MAGIC              0xCafeCeefu
#define SK_PROTOCOL_VERSION   1

#define MAX_WALKEEPERS        32
#define XLOG_HDR_SIZE         (1+8*3)  /* 'w' + startPos + walEnd + timestamp */
#define XLOG_HDR_START_POS    1        /* offset of start position in wal sender message header */
#define XLOG_HDR_END_POS      (1+8)    /* offset of end position in wal sender message header */

extern char* wal_acceptors_list;
extern int   wal_acceptor_reconnect_timeout;
extern bool  am_wal_proposer;

struct WalMessage;
typedef struct WalMessage WalMessage;

extern char *zenith_timeline_walproposer;

/* WAL safekeeper state */
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
} WalKeeperState;

/*
 * Unique node identifier used by Paxos
 */
typedef struct NodeId
{
	uint64     term;
	pg_uuid_t  uuid;
} NodeId;

/*
 * Information about Postgres server broadcasted by WAL proposer to walkeeper
 */
typedef struct ServerInfo
{
	uint32     protocolVersion;   /* proposer-walkeeper protocol version */
	uint32     pgVersion;         /* Postgres server version */
	NodeId     nodeId;
	uint64     systemId;          /* Postgres system identifier */
	uint8	   ztimelineid[16];   /* Zenith timeline id */
	XLogRecPtr walEnd;
    TimeLineID timeline;
	int        walSegSize;
} ServerInfo;

/*
 * Vote request sent from proposer to walkeepers
 */
typedef struct RequestVote
{
	NodeId     nodeId;
	XLogRecPtr VCL;   /* volume commit LSN */
	uint64     epoch; /* new epoch when walkeeper reaches VCL */
} RequestVote;

/*
 * Information of about storage node
 */
typedef struct WalKeeperInfo
{
	uint32     magic;             /* magic for verifying content the control file */
	uint32     formatVersion;     /* walkeeper format version */
	uint64     epoch;             /* walkeeper's epoch */
	ServerInfo server;
	XLogRecPtr commitLsn;         /* part of WAL acknowledged by quorum */
	XLogRecPtr flushLsn;          /* locally flushed part of WAL */
	XLogRecPtr restartLsn;        /* minimal LSN which may be needed for recovery of some walkeeper: min(commitLsn) for all walkeepers */
} WalKeeperInfo;

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
 * Request with WAL message sent from proposer to walkeeper.
 */
typedef struct WalKeeperRequest
{
	NodeId     senderId;    /* Sender's node identifier (looks like we do not need it for TCP streaming connection) */
	XLogRecPtr beginLsn;    /* start position of message in WAL */
	XLogRecPtr endLsn;      /* end position of message in WAL */
	XLogRecPtr restartLsn;  /* restart LSN position  (minimal LSN which may be needed by proposer to perform recovery) */
	XLogRecPtr commitLsn;   /* LSN committed by quorum of walkeepers */
} WalKeeperRequest;

/*
 * All copy data message ('w') are linked in L1 send list and asynchronously sent to receivers.
 * When message is sent to all receivers, it is removed from send list.
 */
struct WalMessage
{
	WalMessage* next;      /* L1 list of messages */
	uint32 size;           /* message size */
	uint32 ackMask;        /* mask of receivers acknowledged receiving of this message */
	WalKeeperRequest req; /* request to walkeeper (message header) */
};

/*
 * Report walkeeper state to proposer
 */
typedef struct WalKeeperResponse
{
	uint64     epoch;
	XLogRecPtr flushLsn;
	HotStandbyFeedback hs;
} WalKeeperResponse;


/*
 * Descriptor of walkeeper
 */
typedef struct WalKeeper
{
    char const* host;
    char const* port;
	pgsocket    sock;     /* socket descriptor */
	WalMessage* currMsg;  /* message been send to the receiver */
	int         asyncOffs;/* offset for asynchronus read/write operations */
	int         eventPos; /* position in wait event set */
	WalKeeperState state;/* walkeeper state machine state */
    WalKeeperInfo  info; /* walkeeper info */
	WalKeeperResponse feedback; /* feedback to master */
} WalKeeper;


int        CompareNodeId(NodeId* id1, NodeId* id2);
pgsocket   ConnectSocketAsync(char const* host, char const* port, bool* established);
bool       WriteSocket(pgsocket sock, void const* buf, size_t size);
ssize_t    ReadSocketAsync(pgsocket sock, void* buf, size_t size);
ssize_t    WriteSocketAsync(pgsocket sock, void const* buf, size_t size);
int        CompareLsn(const void *a, const void *b);
void       WalProposerMain(Datum main_arg);
void       WalProposerBroadcast(XLogRecPtr startpos, char* data, int len);
bool       HexDecodeString(uint8 *result, char *input, int nbytes);
void       WalProposerPoll(void);
void       WalProposerRegister(void);
void       ProcessStandbyReply(XLogRecPtr	writePtr,
							   XLogRecPtr	flushPtr,
							   XLogRecPtr	applyPtr,
							   TimestampTz replyTime,
							   bool		replyRequested);
void       ProcessStandbyHSFeedback(TimestampTz   replyTime,
									TransactionId feedbackXmin,
									uint32		feedbackEpoch,
									TransactionId feedbackCatalogXmin,
									uint32		feedbackCatalogEpoch);
void       StartReplication(StartReplicationCmd *cmd);

#endif
