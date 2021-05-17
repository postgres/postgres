/*-------------------------------------------------------------------------
 *
 * safekeeper_proxy.c - receive WAL stream from master and broadcast it to multiple safekeepers
 *
 * Author: Konstantin Knizhnik <knizhnik@garret.ru>
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/safekeeper/safekeeper_proxy.c
 *-------------------------------------------------------------------------
 */

#include "safekeeper.h"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "receivelog.h"
#include "streamutil.h"

/* Global options */
static int	verbose = 0;
static int  quorum = 0;
static int  n_safekeepers = 0;
static int  reconnect_timeout = 1; /* seconds */

static char*        safekeepersList;
static Safekeeper   safekeeper[MAX_SAFEKEEPERS];
static WalMessage*  msgQueueHead;
static WalMessage*  msgQueueTail;
static XLogRecPtr	lastSentLsn;	/* WAL has been appended to msg queue up to this point */
static XLogRecPtr	lastSentVCLLsn;	/* VCL replies have been sent to safekeeper up to here */
static ServerInfo   serverInfo;
static fd_set       readSet;
static fd_set       writeSet;
static int          maxFds;
static SafekeeperResponse lastFeedback;
static XLogRecPtr   restartLsn; /* Last position received by all safekeepers. */
static RequestVote  prop;       /* Vote request for safekeeper */
static int          leader;     /* Most advanced safekeeper */
static uint8		ztimelineid[16];

static void parse_ztimelineid(char *str);

static WalMessage* CreateMessageVCLOnly(void);
static void BroadcastMessage(WalMessage* msg);

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
SendFeedback(PGconn *conn, XLogRecPtr blockpos, TimestampTz now, bool replyRequested)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int			len = 0;

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(blockpos, &replybuf[len]); /* write */
	len += 8;
	fe_sendint64(blockpos, &replybuf[len]);	/* flush */
	len += 8;
	fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* apply */
	len += 8;
	fe_sendint64(now, &replybuf[len]);	/* sendTime */
	len += 8;
	replybuf[len] = replyRequested ? 1 : 0; /* replyRequested */
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("Could not send feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}

/*
 * Send a hot standby feedback to the master
 */
static bool
SendHSFeedback(PGconn *conn, HotStandbyFeedback* hs)
{
	char		replybuf[1 + 8 + 8 + 8];
	int			len = 0;

	replybuf[len] = 'h';
	len += 1;
	fe_sendint64(hs->ts, &replybuf[len]); /* write */
	len += 8;
	fe_sendint32(XidFromFullTransactionId(hs->xmin), &replybuf[len]);
	len += 4;
	fe_sendint32(EpochFromFullTransactionId(hs->xmin), &replybuf[len]);
	len += 4;
	fe_sendint32(XidFromFullTransactionId(hs->catalog_xmin), &replybuf[len]);
	len += 4;
	fe_sendint32(EpochFromFullTransactionId(hs->catalog_xmin), &replybuf[len]);
	len += 4;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("Could not send hot standby feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}


/*
 * Combine hot standby feedbacks from all safekeepers.
 */
static void
CombineHotStanbyFeedbacks(HotStandbyFeedback* hs)
{
	hs->ts = 0;
	hs->xmin.value = ~0; /* largest unsigned value */
	hs->catalog_xmin.value = ~0; /* largest unsigned value */

	for (int i = 0; i < n_safekeepers; i++)
	{
		if (safekeeper[i].feedback.hs.ts != 0)
		{
			if (FullTransactionIdPrecedes(safekeeper[i].feedback.hs.xmin, hs->xmin))
			{
				hs->xmin = safekeeper[i].feedback.hs.xmin;
				hs->ts = safekeeper[i].feedback.hs.ts;
			}
			if (FullTransactionIdPrecedes(safekeeper[i].feedback.hs.catalog_xmin, hs->catalog_xmin))
			{
				hs->catalog_xmin = safekeeper[i].feedback.hs.catalog_xmin;
				hs->ts = safekeeper[i].feedback.hs.ts;
			}
		}
	}
}

/*
 * This function is called to establish new connection or to reestablish connection in case
 * of connection failure.
 * Close current connection if any and try to initiate new one
 */
static void
ResetConnection(int i)
{
	bool established;

	if (safekeeper[i].state != SS_OFFLINE)
	{
		pg_log_info("Connection with node %s:%s failed: %m",
					safekeeper[i].host, safekeeper[i].port);

		/* Close old connection */
		closesocket(safekeeper[i].sock);
		FD_CLR(safekeeper[i].sock, &writeSet);
		FD_CLR(safekeeper[i].sock, &readSet);
		safekeeper[i].sock = PGINVALID_SOCKET;
		safekeeper[i].state = SS_OFFLINE;
	}

	/* Try to establish new connection */
	safekeeper[i].sock = ConnectSocketAsync(safekeeper[i].host, safekeeper[i].port, &established);
	if (safekeeper[i].sock != PGINVALID_SOCKET)
	{
		pg_log_info("%s with node %s:%s",
					established ? "Connected" : "Connecting", safekeeper[i].host, safekeeper[i].port);
		if (safekeeper[i].sock > maxFds)
			maxFds = safekeeper[i].sock;

		if (established)
		{
			/* Start handshake: first of all send information about server */
			if (WriteSocket(safekeeper[i].sock, &serverInfo, sizeof serverInfo))
			{
				FD_SET(safekeeper[i].sock, &readSet);
				safekeeper[i].state = SS_HANDSHAKE;
				safekeeper[i].asyncOffs = 0;
			}
			else
			{
				ResetConnection(i);
			}
		}
		else
		{
			FD_SET(safekeeper[i].sock, &writeSet);
			safekeeper[i].state = SS_CONNECTING;
		}
	}
}

/*
 * Calculate WAL position acknowledged by quorum
 */
static XLogRecPtr
GetAcknowledgedByQuorumWALPosition(void)
{
	XLogRecPtr responses[MAX_SAFEKEEPERS];
	/*
	 * Sort acknowledged LSNs
	 */
	for (int i = 0; i < n_safekeepers; i++)
	{
		responses[i] = safekeeper[i].feedback.epoch == prop.epoch
			? safekeeper[i].feedback.flushLsn : prop.VCL;
	}
	qsort(responses, n_safekeepers, sizeof(XLogRecPtr), CompareLsn);

	/*
	 * Get the smallest LSN committed by quorum
	 */
	return responses[n_safekeepers - quorum];
}

/*
 * Recompute commitLSN and send feedbacks to master if needed
 */
static bool
HandleSafekeeperResponse(PGconn* conn)
{
	HotStandbyFeedback hsFeedback;
	XLogRecPtr minQuorumLsn = GetAcknowledgedByQuorumWALPosition();

	if (minQuorumLsn > lastFeedback.flushLsn)
	{
		lastFeedback.flushLsn = minQuorumLsn;
		if (!SendFeedback(conn, lastFeedback.flushLsn, feGetCurrentTimestamp(), false))
			return false;
	}
	CombineHotStanbyFeedbacks(&hsFeedback);
	if (hsFeedback.ts != 0 && memcmp(&hsFeedback, &lastFeedback.hs, sizeof hsFeedback) != 0)
	{
		lastFeedback.hs = hsFeedback;
		if (!SendHSFeedback(conn, &hsFeedback))
			return false;
	}

	/* Cleanup message queue */
	while (msgQueueHead != NULL && msgQueueHead->ackMask == ((1 << n_safekeepers) - 1))
	{
		WalMessage* msg = msgQueueHead;
		msgQueueHead = msg->next;
		if (restartLsn < msg->req.beginLsn)
			restartLsn = msg->req.endLsn;
		memset(msg, 0xDF, sizeof(WalMessage) + msg->size - sizeof(SafekeeperRequest));
		pg_free(msg);
	}
	if (!msgQueueHead) /* queue is empty */
		msgQueueTail = NULL;

	return true;
}


static void
usage(void)
{
	printf(_("%s tee PostgreSQL streaming write-ahead logs.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -q, --quorum            quorum for sending response to server\n"));
	printf(_("  -s, --safekeepers       comma separated list of safekeeprs in format 'host1:port1,host2:port2'\n"));
	printf(_("  -r, --reconnect-timeout timeout for reconnection attempt to offline safekeepers\n"));
	printf(_("  -v, --verbose           output verbose messages\n"));
	printf(_("  -V, --version           output version information, then exit\n"));
	printf(_("  -?, --help              show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR    connection string\n"));
	printf(_("  -h, --host=HOSTNAME     database server host or socket directory\n"));
	printf(_("  -p, --port=PORT         database server port number\n"));
	printf(_("  -U, --username=NAME     connect as specified database user\n"));
	printf(_("  -w, --no-password       never prompt for password\n"));
	printf(_("  -W, --password          force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


/*
 * Send message to the particular node
 */
static void
SendMessageToNode(int i, WalMessage* msg)
{
	ssize_t rc;

	/* If there is no pending message then send new one */
	if (safekeeper[i].currMsg == NULL)
	{
		/* Skip already acknowledged messages */
		while (msg != NULL && (msg->ackMask & (1 << i)) != 0)
			msg = msg->next;

		safekeeper[i].currMsg = msg;
	}
	else
		msg = safekeeper[i].currMsg;

	if (msg != NULL)
	{
		msg->req.restartLsn = restartLsn;
		msg->req.commitLsn = GetAcknowledgedByQuorumWALPosition();

		pg_log_info("sending message with len %ld VCL=%X/%X to %d",
					msg->size - sizeof(SafekeeperRequest),
					(uint32) (msg->req.commitLsn >> 32), (uint32) msg->req.commitLsn, i);

		rc = WriteSocketAsync(safekeeper[i].sock, &msg->req, msg->size);
		if (rc < 0)
		{
			ResetConnection(i);
		}
		else if ((size_t)rc == msg->size) /* message was completely sent */
		{
			safekeeper[i].asyncOffs = 0;
			safekeeper[i].state = SS_RECV_FEEDBACK;
		}
		else
		{
			/* wait until socket is available for write */
			safekeeper[i].state = SS_SEND_WAL;
			safekeeper[i].asyncOffs = rc;
			FD_SET(safekeeper[i].sock, &writeSet);
		}
	}
}

/*
 * Broadcast new message to all caught-up safekeepers
 */
static void
BroadcastMessage(WalMessage* msg)
{
	for (int i = 0; i < n_safekeepers; i++)
	{
		if (safekeeper[i].state == SS_IDLE && safekeeper[i].currMsg == NULL)
		{
			SendMessageToNode(i, msg);
		}
	}
}


/*
 * Send termination message to safekeepers
 */
static void
StopSafekeepers(void)
{
	SafekeeperRequest req;
	req.senderId = prop.nodeId;
	req.beginLsn = END_OF_STREAM;
	req.endLsn = END_OF_STREAM;

	Assert(!msgQueueHead); /* there should be no pending messages */

	for (int i = 0; i < n_safekeepers; i++)
	{
		if (safekeeper[i].sock != PGINVALID_SOCKET)
		{
			WriteSocket(safekeeper[i].sock, &req, sizeof(req));
			closesocket(safekeeper[i].sock);
			safekeeper[i].sock = PGINVALID_SOCKET;
		}
	}
}

/*
 * Create WAL message from received COPY data and link it into queue
 */
static WalMessage*
CreateMessage(char* data, int len)
{
	/* Create new message and append it to message queue */
	WalMessage*	msg;
	XLogRecPtr startpos = fe_recvint64(&data[XLOG_HDR_START_POS]);
	XLogRecPtr endpos = fe_recvint64(&data[XLOG_HDR_END_POS]);;

	len -= XLOG_HDR_SIZE; /* skip message header */
	endpos = startpos + len;
	if (msgQueueTail && msgQueueTail->req.endLsn >= endpos)
	{
		/* Message already queued */
		return NULL;
	}
	Assert(len >= 0);
	msg = (WalMessage*)pg_malloc(sizeof(WalMessage) + len);
	if (msgQueueTail != NULL)
		msgQueueTail->next = msg;
	else
		msgQueueHead = msg;
	msgQueueTail = msg;

	msg->size = sizeof(SafekeeperRequest) + len;
	msg->next = NULL;
	msg->ackMask = 0;
	msg->req.beginLsn = startpos;
	msg->req.endLsn = endpos;
	msg->req.senderId = prop.nodeId;
	memcpy(&msg->req+1, data + XLOG_HDR_SIZE, len);
	PQfreemem(data);

	Assert(msg->req.endLsn >= lastSentLsn);
	lastSentLsn = msg->req.endLsn;
	return msg;
}

/*
 * Create WAL message with no data, just to let the safekeepers
 * know that the VCL has advanced.
 */
static WalMessage*
CreateMessageVCLOnly(void)
{
	/* Create new message and append it to message queue */
	WalMessage*	msg;

	if (lastSentLsn == 0)
	{
		/* FIXME: We haven't sent anything yet. Not sure what to do then.. */
		return NULL;
	}

	msg = (WalMessage*)pg_malloc(sizeof(WalMessage));
	if (msgQueueTail != NULL)
		msgQueueTail->next = msg;
	else
		msgQueueHead = msg;
	msgQueueTail = msg;

	msg->size = sizeof(SafekeeperRequest);
	msg->next = NULL;
	msg->ackMask = 0;
	msg->req.beginLsn = lastSentLsn;
	msg->req.endLsn = lastSentLsn;
	msg->req.senderId = prop.nodeId;
	/* restartLsn and commitLsn are set just before the message sent, in SendMessageToNode() */
	return msg;
}

/*
 * Synchronize state of safekeepers.
 * We will find most advanced safekeeper within quorum and download
 * from it WAL from max(restartLsn) till max(flushLsn).
 * Then we adjust message queue to populate rest safekeepers with missed WAL.
 * It enforces the rule that there are no "alternative" versions of WAL in safekeepers.
 * Before any record from new epoch can reach safekeeper, we enforce that all WAL records fro prior epochs
 * are pushed here.
 */
static bool
StartRecovery(void)
{
	if (verbose)
		pg_log_info("Restart LSN=%X/%X, VCL=%X/%X",
					(uint32) (restartLsn >> 32), (uint32)restartLsn,
					(uint32) (prop.VCL >> 32), (uint32)prop.VCL);

	if (restartLsn != prop.VCL) /* if not all safekeepers are up-to-date, we need to download WAL needed to synchronize them */
	{
		PGresult  *res;
		WalMessage* msg;
		char query[256];
		PGconn* conn = ConnectSafekeeper(safekeeper[leader].host,
										 safekeeper[leader].port);
		if (!conn)
			return false;

		if (verbose)
			pg_log_info("Start retrieve of missing WALs from %s:%s from %X/%X till %X/%X",
						safekeeper[leader].host, safekeeper[leader].port,
						(uint32) (restartLsn >> 32), (uint32)restartLsn,
						(uint32) (prop.VCL >> 32), (uint32)prop.VCL);

		snprintf(query, sizeof(query), "START_REPLICATION %X/%X TIMELINE %u TILL %X/%X", /* TILL is safekeeper extension of START_REPLICATION command */
				 (uint32) (restartLsn >> 32), (uint32)restartLsn,
				 serverInfo.timeline,
				 (uint32) (prop.VCL >> 32), (uint32)prop.VCL);
		res = PQexec(conn, query);
		if (PQresultStatus(res) != PGRES_COPY_BOTH)
		{
			pg_log_error("could not send replication command \"%s\": %s",
						 "START_REPLICATION", PQresultErrorMessage(res));
			PQclear(res);
			PQfinish(conn);
			return false;
		}
		/*
		 * Receive WAL from most advanced safekeeper. As far as connection quorum may be different from last commit quorum,
		 * we can not conclude weather last wal record was committed or not. So we assume that it is committed and replicate it
		 * to all all safekeepers.
		 */
		do
		{
			char* copybuf;
			int rawlen = PQgetCopyData(conn, &copybuf, 0);
			if (rawlen <= 0)
			{
				if (rawlen == -2)
					pg_log_error("Could not read COPY data from %s:%s: %s",
								 safekeeper[leader].host,
								 safekeeper[leader].port,
								 PQerrorMessage(conn));
				else
					pg_log_info("End of WAL stream from %s:%s reached",
								safekeeper[leader].host,
								safekeeper[leader].port);
				PQfinish(conn);
				return false;
			}
			Assert (copybuf[0] == 'w');
			msg = CreateMessage(copybuf, rawlen);
		} while (msg && msg->req.endLsn < prop.VCL); /* loop until we reach last flush position */

		/* Setup restart point for all safekeepers */
		for (int i = 0; i < n_safekeepers; i++)
		{
			if (safekeeper[i].state == SS_IDLE)
			{
				for (msg = msgQueueHead; msg != NULL; msg = msg->next)
				{
					if (msg->req.endLsn <= safekeeper[i].info.flushLsn)
					{
						msg->ackMask |= 1 << i; /* message is already received by this safekeeper */
					}
					else
					{
						SendMessageToNode(i, msg);
						break;
					}
				}
			}
		}
		if (verbose)
			pg_log_info("Recovery completed");
	}
	return true;
}


/*
 * Prepare vote request for election
 */
static void
StartElection(void)
{
	XLogRecPtr initWALPos = serverInfo.walSegSize;
	prop.VCL = restartLsn = initWALPos;
	prop.nodeId = serverInfo.nodeId;
	for (int i = 0; i < n_safekeepers; i++)
	{
		if (safekeeper[i].state == SS_VOTING)
		{
			prop.nodeId.term = Max(safekeeper[i].info.server.nodeId.term, prop.nodeId.term);
			restartLsn = Max(safekeeper[i].info.restartLsn, restartLsn);
			if (safekeeper[i].info.epoch > prop.epoch
				|| (safekeeper[i].info.epoch == prop.epoch && safekeeper[i].info.flushLsn > prop.VCL))

			{
				prop.epoch = safekeeper[i].info.epoch;
				prop.VCL = safekeeper[i].info.flushLsn;
				leader = i;
			}
		}
	}
	/* Only safekeepers from most recent epoch can report it's FlushLsn to master */
	for (int i = 0; i < n_safekeepers; i++)
	{
		if (safekeeper[i].state == SS_VOTING)
		{
			if (safekeeper[i].info.epoch == prop.epoch)
			{
				safekeeper[i].feedback.flushLsn = safekeeper[i].info.flushLsn;
			}
			else if (verbose)
			{
				pg_log_info("Safekeeper %s:%s belongs to old epoch " INT64_FORMAT " while current epoch is " INT64_FORMAT,
							safekeeper[i].host,
							safekeeper[i].port,
							safekeeper[i].info.epoch,
							prop.epoch);
			}
		}
	}
	prop.nodeId.term += 1;
	prop.epoch += 1;
}

/*
 * Start WAL sender at master
 */
static bool
StartReplication(PGconn* conn)
{
	char	   query[128];
	PGresult  *res;
	XLogRecPtr startpos = prop.VCL;

	/*
	 * Always start streaming at the beginning of a segment
	 */
	startpos -= XLogSegmentOffset(startpos, serverInfo.walSegSize);

	/* Initiate the replication stream at specified location */
	snprintf(query, sizeof(query), "START_REPLICATION %X/%X TIMELINE %u",
			 (uint32) (startpos >> 32), (uint32)startpos,
			 serverInfo.timeline);
	if (verbose)
		pg_log_info("%s", query);
	res = PQexec(conn, query);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 "START_REPLICATION", PQresultErrorMessage(res));
		PQclear(res);
		return false;
	}
	PQclear(res);
	return true;
}

/*
 * WAL broadcasting loop
 */
static void
BroadcastWalStream(PGconn* conn)
{
	pgsocket server = PQsocket(conn);
	bool     streaming = true;
	int      i;
	ssize_t  rc;
	int      n_votes = 0;
	int      n_connected = 0;
	time_t   last_reconnect_attempt = time(NULL);

	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);
	maxFds = server;

	/* Initiate connections to all safekeeper nodes */
	for (i = 0; i < n_safekeepers; i++)
	{
		ResetConnection(i);
	}

	while (streaming || msgQueueHead != NULL) /* continue until server is streaming WAL or we have some unacknowledged messages */
	{
		fd_set rs = readSet;
		fd_set ws = writeSet;
		struct timeval tv;
		time_t now;
		tv.tv_sec = reconnect_timeout;
		tv.tv_usec = 0;
		rc = select(maxFds+1, &rs, &ws, NULL, reconnect_timeout > 0 ? &tv : NULL);
		if (rc < 0)
		{
			pg_log_error("Select failed: %m");
			break;
		}
		/* Initiate reconnect if timeout is expired */
		now = time(NULL);
		if (reconnect_timeout > 0 && now - last_reconnect_attempt > reconnect_timeout)
		{
			last_reconnect_attempt = now;
			for (i = 0; i < n_safekeepers; i++)
			{
				if (safekeeper[i].state == SS_OFFLINE)
					ResetConnection(i);
			}
		}
		if (server != PGINVALID_SOCKET && FD_ISSET(server, &rs)) /* New message from server */
		{
			bool async = false;
			while (true)
			{
				char* copybuf;
				int rawlen;

				if (async)
				{
					if (PQconsumeInput(conn) != 1)
					{
						pg_log_error("Could not read COPY data: %s", PQerrorMessage(conn));
						FD_CLR(server, &readSet);
						closesocket(server);
						server = PGINVALID_SOCKET;
						streaming = false;
						break;
					}
				}
				rawlen = PQgetCopyData(conn, &copybuf, async);
				if (rawlen == 0)
				{
					/* no more data available */
					break;
				}
				else if (rawlen < 0)
				{
					if (rawlen == -2)
						pg_log_error("Could not read COPY data: %s", PQerrorMessage(conn));
					else
						pg_log_info("End of WAL stream reached");
					FD_CLR(server, &readSet);
					closesocket(server);
					server = PGINVALID_SOCKET;
					streaming = false;
					break;
				}
				if (copybuf[0] == 'w')
				{
					WalMessage* msg = CreateMessage(copybuf, rawlen);
					if (msg != NULL)
						BroadcastMessage(msg);
				}
				else
				{
					Assert(copybuf[0] == 'k'); /* keep alive */
					if (copybuf[KEEPALIVE_RR_OFFS] /* response requested */
						&& !SendFeedback(conn, lastFeedback.flushLsn, feGetCurrentTimestamp(), false))
					{
						FD_CLR(server, &readSet);
						closesocket(server);
						server = PGINVALID_SOCKET;
						streaming = false;
					}
					PQfreemem(copybuf);
				}
				async = true;
			}
		}
		/* communication with safekeepers */
		{
			for (int i = 0; i < n_safekeepers; i++)
			{
				if (safekeeper[i].sock == PGINVALID_SOCKET)
					continue;
				if (FD_ISSET(safekeeper[i].sock, &rs))
				{
					switch (safekeeper[i].state)
					{
					  case SS_HANDSHAKE:
					  {
						  /* Receive safekeeper node state */
						  rc = ReadSocketAsync(safekeeper[i].sock,
											   (char*)&safekeeper[i].info + safekeeper[i].asyncOffs,
											   sizeof(safekeeper[i].info) - safekeeper[i].asyncOffs);
						  if (rc < 0)
						  {
							  ResetConnection(i);
						  }
						  else if ((safekeeper[i].asyncOffs += rc) == sizeof(safekeeper[i].info))
						  {
							  /* Safekeeper response completely received */

							  /* Check protocol version */
							  if (safekeeper[i].info.server.protocolVersion != SK_PROTOCOL_VERSION)
							  {
								  pg_log_error("Safekeeper has incompatible protocol version %d vs. %d",
											   safekeeper[i].info.server.protocolVersion, SK_PROTOCOL_VERSION);
								  ResetConnection(i);
							  }
							  else
							  {
								  safekeeper[i].state = SS_VOTING;
								  safekeeper[i].feedback.flushLsn = restartLsn;
								  safekeeper[i].feedback.hs.ts = 0;

								  /* Check if we have quorum */
								  if (++n_connected >= quorum)
								  {
									  if (n_connected == quorum)
										  StartElection();

									  /* Now send max-node-id to everyone participating in voting and wait their responses */
									  for (int j = 0; j < n_safekeepers; j++)
									  {
										  if (safekeeper[j].state == SS_VOTING)
										  {
											  if (!WriteSocket(safekeeper[j].sock, &prop, sizeof(prop)))
											  {
												  ResetConnection(j);
											  }
											  else
											  {
												  safekeeper[j].asyncOffs = 0;
												  safekeeper[j].state = SS_WAIT_VERDICT;
											  }
										  }
									  }
								  }
							  }
						  }
						  break;
					  }
					  case SS_WAIT_VERDICT:
					  {
						  /* Receive safekeeper response for our candidate */
						  rc = ReadSocketAsync(safekeeper[i].sock,
											   (char*)&safekeeper[i].info.server.nodeId + safekeeper[i].asyncOffs,
											   sizeof(safekeeper[i].info.server.nodeId) - safekeeper[i].asyncOffs);
						  if (rc < 0)
						  {
							  ResetConnection(i);
						  }
						  else if ((safekeeper[i].asyncOffs += rc) == sizeof(safekeeper[i].info.server.nodeId))
						  {
							  /* Response completely received */

							  /* If server accept our candidate, then it returns it in response */
							  if (CompareNodeId(&safekeeper[i].info.server.nodeId, &prop.nodeId) != 0)
							  {
								  pg_log_error("SafeKeeper %s:%s with term " INT64_FORMAT " rejects our connection request with term " INT64_FORMAT "",
											   safekeeper[i].host, safekeeper[i].port,
											   safekeeper[i].info.server.nodeId.term, prop.nodeId.term);
								  exit(1);
							  }
							  else
							  {
								  /* Handshake completed, do we have quorum? */
								  safekeeper[i].state = SS_IDLE;
								  if (++n_votes == quorum)
								  {
									  if (verbose)
										  pg_log_info("Successfully established connection with %d nodes", quorum);

									  /* Perform recovery */
									  if (!StartRecovery())
										  exit(1);

									  /* Start replication from master */
									  if (StartReplication(conn))
									  {
										  FD_SET(server, &readSet);
									  }
									  else
									  {
										  exit(1);
									  }
								  }
								  else
								  {
									  /* We are already streaming WAL: send all pending messages to the attached safekeeper */
									  SendMessageToNode(i, msgQueueHead);
								  }
							  }
						  }
						  break;
					  }
					  case SS_RECV_FEEDBACK:
					  {
						  /* Read safekeeper response with flushed WAL position */
						  rc = ReadSocketAsync(safekeeper[i].sock,
											   (char*)&safekeeper[i].feedback + safekeeper[i].asyncOffs,
											   sizeof(safekeeper[i].feedback) - safekeeper[i].asyncOffs);
						  if (rc < 0)
						  {
							  ResetConnection(i);
						  }
						  else if ((safekeeper[i].asyncOffs += rc) == sizeof(safekeeper[i].feedback))
						  {
							  WalMessage* next = safekeeper[i].currMsg->next;
							  Assert(safekeeper[i].feedback.flushLsn == safekeeper[i].currMsg->req.endLsn);
							  safekeeper[i].currMsg->ackMask |= 1 << i; /* this safekeeper confirms receiving of this message */
							  safekeeper[i].state = SS_IDLE;
							  safekeeper[i].asyncOffs = 0;
							  safekeeper[i].currMsg = NULL;
							  if (!HandleSafekeeperResponse(conn))
							  {
								  FD_CLR(server, &readSet);
								  closesocket(server);
								  server = PGINVALID_SOCKET;
								  streaming = false;
							  }
							  else
							  {
								  SendMessageToNode(i, next);

								  /*
								   * Also send the new VCL to all the safekeepers.
								   *
								   * FIXME: This is redundant for safekeepers that have other outbound messages
								   * pending.
								   */
								  if (1)
								  {
									  XLogRecPtr minQuorumLsn = GetAcknowledgedByQuorumWALPosition();
									  WalMessage *vclUpdateMsg;

									  if (minQuorumLsn > lastSentVCLLsn)
									  {
										  vclUpdateMsg = CreateMessageVCLOnly();
										  if (vclUpdateMsg)
											  BroadcastMessage(vclUpdateMsg);
										  lastSentVCLLsn = minQuorumLsn;
									  }
								  }
							  }
						  }
						  break;
					  }
					  case SS_IDLE:
					  {
						  pg_log_info("Safekeeper %s:%s drops connection", safekeeper[i].host, safekeeper[i].port);
						  ResetConnection(i);
						  break;
					  }
					  default:
					  {
						  pg_log_error("Unexpected safekeeper %s:%s read state %d", safekeeper[i].host, safekeeper[i].port, safekeeper[i].state);
						  exit(1);
					  }
					}
				}
				else if (FD_ISSET(safekeeper[i].sock, &ws))
				{
					switch (safekeeper[i].state)
					{
					  case SS_CONNECTING:
					  {
						  int			optval = 0;
						  ACCEPT_TYPE_ARG3 optlen = sizeof(optval);
						  if (getsockopt(safekeeper[i].sock, SOL_SOCKET, SO_ERROR, (char *) &optval, &optlen) < 0 || optval != 0)
						  {
							  pg_log_error("Failed to connect to node '%s:%s': %s",
										   safekeeper[i].host, safekeeper[i].port,
										   strerror(optval));
							  closesocket(safekeeper[i].sock);
							  FD_CLR(safekeeper[i].sock, &writeSet);
							  safekeeper[i].sock =  PGINVALID_SOCKET;
							  safekeeper[i].state = SS_OFFLINE;
						  }
						  else
						  {
							  uint32 len = 0;
							  FD_CLR(safekeeper[i].sock, &writeSet);
							  FD_SET(safekeeper[i].sock, &readSet);
							  /*
							   * Start handshake: send information about server.
							   * First of all send 0 as package size: it allows safekeeper to distinguish
							   * connection from safekeeper_proxy from standard replication connection from pagers.
							   */
							  if (WriteSocket(safekeeper[i].sock, &len, sizeof len)
								  && WriteSocket(safekeeper[i].sock, &serverInfo, sizeof serverInfo))
							  {
								  safekeeper[i].state = SS_HANDSHAKE;
								  safekeeper[i].asyncOffs = 0;
							  }
							  else
							  {
								  ResetConnection(i);
							  }
						  }
						  break;
					  }
					  case SS_SEND_WAL:
					  {
						  rc = WriteSocketAsync(safekeeper[i].sock, (char*)&safekeeper[i].currMsg->req + safekeeper[i].asyncOffs, safekeeper[i].currMsg->size - safekeeper[i].asyncOffs);
						  if (rc < 0)
						  {
							  ResetConnection(i);
						  }
						  else if ((safekeeper[i].asyncOffs += rc) == safekeeper[i].currMsg->size)
						  {
							  /* WAL block completely sent */
							  safekeeper[i].state = SS_RECV_FEEDBACK;
							  safekeeper[i].asyncOffs = 0;
							  FD_CLR(safekeeper[i].sock, &writeSet);
						  }
						  break;
					  }
					  default:
						pg_log_error("Unexpected write state %d", safekeeper[i].state);
						exit(1);
					}
				}
			}
		}
	}
	StopSafekeepers();
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"reconnect-timeout", required_argument, NULL, 'r'},
		{"dbname", required_argument, NULL, 'd'},
		{"safekeepers", required_argument, NULL, 's'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"verbose", no_argument, NULL, 'v'},
		{"ztimelineid", required_argument, NULL, 1},
		{NULL, 0, NULL, 0}
	};

	int			c;
	int			option_index;
	char	   *db_name;
	PGconn 	   *conn;
	char       *host;
	char       *port;
	char       *sep;
	char       *sysid;
	char	   *ztimelineid_arg = NULL;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("safekeeper"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0 ||
				 strcmp(argv[1], "--version") == 0)
		{
			puts("safekeeper (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "d:h:p:U:s:vwW",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'd':
				connection_string = pg_strdup(optarg);
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'p':
				if (atoi(optarg) <= 0)
				{
					pg_log_error("invalid port number \"%s\"", optarg);
					exit(1);
				}
				dbport = pg_strdup(optarg);
				break;
			case 'U':
				dbuser = pg_strdup(optarg);
				break;
			case 's':
				safekeepersList = pg_strdup(optarg);
				break;
			case 'r':
				reconnect_timeout = atoi(optarg);
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
			case 'v':
				verbose++;
				break;
			case 1:
				ztimelineid_arg = pg_strdup(optarg);
				break;
			default:

				/*
				 * getopt_long already emitted a complaint
				 */
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	/*
	 * Any non-option arguments?
	 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	for (host = safekeepersList; host != NULL && *host != '\0'; host = sep)
	{
		port = strchr(host, ':');
		if (port == NULL) {
			pg_log_error("port is not specified");
			exit(1);
		}
		*port++ = '\0';
		sep = strchr(port, ',');
		if (sep != NULL)
			*sep++ = '\0';
		if (n_safekeepers+1 >= MAX_SAFEKEEPERS)
		{
			pg_log_error("Too many safekeepers");
			exit(1);
		}
		safekeeper[n_safekeepers].host = host;
		safekeeper[n_safekeepers].port = port;
		safekeeper[n_safekeepers].state = SS_OFFLINE;
		safekeeper[n_safekeepers].sock = PGINVALID_SOCKET;
		safekeeper[n_safekeepers].currMsg = NULL;
		n_safekeepers += 1;
	}
	if (n_safekeepers < 1)
	{
		pg_log_error("Safekeepers addresses are not specified");
		exit(1);
	}
	if (quorum == 0)
	{
		quorum = n_safekeepers/2 + 1;
	}
	else if (quorum < n_safekeepers/2 + 1 || quorum > n_safekeepers)
	{
		pg_log_error("Invalid quorum value: %d, should be %d..%d", quorum, n_safekeepers/2 + 1, n_safekeepers);
		exit(1);
	}

	/* Parse the zenith timeline id */
	if (!ztimelineid_arg)
	{
		pg_log_error("--ztimelineid is required");
		exit(1);
	}
	parse_ztimelineid(ztimelineid_arg);

	/*
	 * Obtain a connection before doing anything.
	 */
	conn = GetConnection();
	if (!conn)
		/* error message already written in GetConnection() */
		exit(1);
	atexit(disconnect_atexit);

	/*
	 * Run IDENTIFY_SYSTEM to make sure we've successfully have established a
	 * replication connection and haven't connected using a database specific
	 * connection.
	 */
	if (!RunIdentifySystem(conn, &sysid, &serverInfo.timeline, &serverInfo.walEnd, &db_name))
		exit(1);

	/* determine remote server's xlog segment size */
	if (!RetrieveWalSegSize(conn))
		exit(1);

	/* Fill information about server */
	serverInfo.walSegSize = WalSegSz;
	serverInfo.pgVersion = PG_VERSION_NUM;
	memcpy(serverInfo.ztimelineid, ztimelineid, 16);
	serverInfo.protocolVersion = SK_PROTOCOL_VERSION;
	pg_strong_random(&serverInfo.nodeId.uuid, sizeof(serverInfo.nodeId.uuid));
	sscanf(sysid, INT64_FORMAT, &serverInfo.systemId);

	/*
	 * Check that there is a database associated with connection, none should
	 * be defined in this context.
	 */
	if (db_name)
	{
		pg_log_error("replication connection is unexpectedly database specific");
		exit(1);
	}

	BroadcastWalStream(conn);

	PQfinish(conn);

	return 0;
}

/*
 * Convert a character which represents a hexadecimal digit to an integer.
 *
 * Returns -1 if the character is not a hexadecimal digit.
 */
static int
hexdecode_char(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/*
 * Decode a hex string into a byte string, 2 hex chars per byte.
 *
 * Returns false if invalid characters are encountered; otherwise true.
 */
static bool
hexdecode_string(uint8 *result, char *input, int nbytes)
{
	int			i;

	for (i = 0; i < nbytes; ++i)
	{
		int			n1 = hexdecode_char(input[i * 2]);
		int			n2 = hexdecode_char(input[i * 2 + 1]);

		if (n1 < 0 || n2 < 0)
			return false;
		result[i] = n1 * 16 + n2;
	}

	return true;
}


static void
parse_ztimelineid(char *str)
{
	if (!hexdecode_string(ztimelineid, str, 16))
	{
		pg_log_error("Could not parse --ztimelineid parameter");
		exit(1);
	}
}
