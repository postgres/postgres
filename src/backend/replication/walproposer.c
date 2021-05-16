/*-------------------------------------------------------------------------
 *
 * walproposer.c
 *
 * Broadcast WAL stream to Zenith WAL acceptetors
 */
#include <signal.h>
#include <unistd.h>
#include "replication/walproposer.h"
#include "storage/latch.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "access/xlog.h"
#include "replication/walreceiver.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/pmsignal.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

char* wal_acceptors_list;
int   wal_acceptor_reconnect_timeout;
bool  am_wal_proposer;

static int          n_walkeepers = 0;
static int          quorum = 0;
static WalKeeper    walkeeper[MAX_WALKEEPERS];
static WalMessage*  msgQueueHead;
static WalMessage*  msgQueueTail;
static XLogRecPtr	lastSentLsn;	/* WAL has been appended to msg queue up to this point */
static XLogRecPtr	lastSentVCLLsn;	/* VCL replies have been sent to walkeeper up to here */
static ServerInfo   serverInfo;
static WaitEventSet* waitEvents;
static WalKeeperResponse lastFeedback;
static XLogRecPtr   restartLsn; /* Last position received by all walkeepers. */
static RequestVote  prop;       /* Vote request for walkeeper */
static int          leader;     /* Most advanced walkeeper */
static int          n_votes = 0;
static int          n_connected = 0;
static TimestampTz  last_reconnect_attempt;

/*
 * Combine hot standby feedbacks from all walkeepers.
 */
static void
CombineHotStanbyFeedbacks(HotStandbyFeedback* hs)
{
	hs->ts = 0;
	hs->xmin.value = ~0; /* largest unsigned value */
	hs->catalog_xmin.value = ~0; /* largest unsigned value */

	for (int i = 0; i < n_walkeepers; i++)
	{
		if (walkeeper[i].feedback.hs.ts != 0)
		{
			if (FullTransactionIdPrecedes(walkeeper[i].feedback.hs.xmin, hs->xmin))
			{
				hs->xmin = walkeeper[i].feedback.hs.xmin;
				hs->ts = walkeeper[i].feedback.hs.ts;
			}
			if (FullTransactionIdPrecedes(walkeeper[i].feedback.hs.catalog_xmin, hs->catalog_xmin))
			{
				hs->catalog_xmin = walkeeper[i].feedback.hs.catalog_xmin;
				hs->ts = walkeeper[i].feedback.hs.ts;
			}
		}
	}
}

static void
ResetWalProposerEventSet(void)
{
	if (waitEvents)
		FreeWaitEventSet(waitEvents);
	waitEvents = CreateWaitEventSet(TopMemoryContext, 2 + n_walkeepers);
	AddWaitEventToSet(waitEvents, WL_LATCH_SET, PGINVALID_SOCKET,
					  MyLatch, NULL);
	AddWaitEventToSet(waitEvents, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
					  NULL, NULL);
	for (int i = 0; i < n_walkeepers; i++)
	{
		if (walkeeper[i].sock != PGINVALID_SOCKET)
		{
			int events;
			switch (walkeeper[i].state)
			{
				case SS_SEND_WAL:
					events = WL_SOCKET_READABLE|WL_SOCKET_WRITEABLE;
					break;
				case SS_CONNECTING:
					events = WL_SOCKET_WRITEABLE;
					break;
				default:
					events = WL_SOCKET_READABLE;
					break;
			}
			walkeeper[i].eventPos = AddWaitEventToSet(waitEvents, events, walkeeper[i].sock, NULL, &walkeeper[i]);
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

	if (walkeeper[i].state != SS_OFFLINE)
	{
		elog(WARNING, "Connection with node %s:%s failed: %m",
			walkeeper[i].host, walkeeper[i].port);

		/* Close old connection */
		closesocket(walkeeper[i].sock);
		walkeeper[i].sock = PGINVALID_SOCKET;
		walkeeper[i].state = SS_OFFLINE;

		/* Postgres wait event set API doesn't support deletion of events, so we have to reconstruct set */
		ResetWalProposerEventSet();
	}

	/* Try to establish new connection */
	walkeeper[i].sock = ConnectSocketAsync(walkeeper[i].host, walkeeper[i].port, &established);
	if (walkeeper[i].sock != PGINVALID_SOCKET)
	{
		elog(LOG, "%s with node %s:%s",
					established ? "Connected" : "Connecting", walkeeper[i].host, walkeeper[i].port);


		if (established)
		{
			/* Start handshake: first of all send information about server */
			if (WriteSocket(walkeeper[i].sock, &serverInfo, sizeof serverInfo))
			{
				walkeeper[i].eventPos = AddWaitEventToSet(waitEvents, WL_SOCKET_READABLE, walkeeper[i].sock, NULL, &walkeeper[i]);
				walkeeper[i].state = SS_HANDSHAKE;
				walkeeper[i].asyncOffs = 0;
			}
			else
			{
				ResetConnection(i);
			}
		}
		else
		{
			walkeeper[i].eventPos = AddWaitEventToSet(waitEvents, WL_SOCKET_WRITEABLE, walkeeper[i].sock, NULL, &walkeeper[i]);
			walkeeper[i].state = SS_CONNECTING;
		}
	}
}


/*
 * Calculate WAL position acknowledged by quorum
 */
static XLogRecPtr
GetAcknowledgedByQuorumWALPosition(void)
{
	XLogRecPtr responses[MAX_WALKEEPERS];
	/*
	 * Sort acknowledged LSNs
	 */
	for (int i = 0; i < n_walkeepers; i++)
	{
		responses[i] = walkeeper[i].feedback.epoch == prop.epoch
			? walkeeper[i].feedback.flushLsn : prop.VCL;
	}
	qsort(responses, n_walkeepers, sizeof(XLogRecPtr), CompareLsn);

	/*
	 * Get the smallest LSN committed by quorum
	 */
	return responses[n_walkeepers - quorum];
}

static void
HandleWalKeeperResponse(void)
{
	HotStandbyFeedback hsFeedback;
	XLogRecPtr minQuorumLsn;

	minQuorumLsn = GetAcknowledgedByQuorumWALPosition();
	if (minQuorumLsn > lastFeedback.flushLsn)
	{
		lastFeedback.flushLsn = minQuorumLsn;
		ProcessStandbyReply(minQuorumLsn, minQuorumLsn, InvalidXLogRecPtr, GetCurrentTimestamp(), false);
	}
	CombineHotStanbyFeedbacks(&hsFeedback);
	if (hsFeedback.ts != 0 && memcmp(&hsFeedback, &lastFeedback.hs, sizeof hsFeedback) != 0)
	{
		lastFeedback.hs = hsFeedback;
		ProcessStandbyHSFeedback(hsFeedback.ts,
								 XidFromFullTransactionId(hsFeedback.xmin),
								 EpochFromFullTransactionId(hsFeedback.xmin),
								 XidFromFullTransactionId(hsFeedback.catalog_xmin),
								 EpochFromFullTransactionId(hsFeedback.catalog_xmin));
	}


	/* Cleanup message queue */
	while (msgQueueHead != NULL && msgQueueHead->ackMask == ((1 << n_walkeepers) - 1))
	{
		WalMessage* msg = msgQueueHead;
		msgQueueHead = msg->next;
		if (restartLsn < msg->req.beginLsn)
			restartLsn = msg->req.endLsn;
		memset(msg, 0xDF, sizeof(WalMessage) + msg->size - sizeof(WalKeeperRequest));
		free(msg);
	}
	if (!msgQueueHead) /* queue is empty */
		msgQueueTail = NULL;
}

char *zenith_timeline_walproposer = NULL;

/*
 * WAL proposer bgworeker entry point
 */
void
WalProposerMain(Datum main_arg)
{
	char* host;
	char* sep;
	char* port;

	/* Establish signal handlers. */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);

		/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false);
	if (WalReceiverFunctions == NULL)
		elog(ERROR, "libpqwalreceiver didn't initialize correctly");

	load_file("zenith", false);

	BackgroundWorkerUnblockSignals();

	for (host = wal_acceptors_list; host != NULL && *host != '\0'; host = sep)
	{
		port = strchr(host, ':');
		if (port == NULL) {
			elog(FATAL, "port is not specified");
		}
		*port++ = '\0';
		sep = strchr(port, ',');
		if (sep != NULL)
			*sep++ = '\0';
		if (n_walkeepers+1 >= MAX_WALKEEPERS)
		{
			elog(FATAL, "Too many walkeepers");
		}
		walkeeper[n_walkeepers].host = host;
		walkeeper[n_walkeepers].port = port;
		walkeeper[n_walkeepers].state = SS_OFFLINE;
		walkeeper[n_walkeepers].sock = PGINVALID_SOCKET;
		walkeeper[n_walkeepers].currMsg = NULL;
		n_walkeepers += 1;
	}
	if (n_walkeepers < 1)
	{
		elog(FATAL, "WalKeepers addresses are not specified");
	}
	quorum = n_walkeepers/2 + 1;

	GetXLogReplayRecPtr(&ThisTimeLineID);

	/* Fill information about server */
	serverInfo.timeline = ThisTimeLineID;
	serverInfo.walEnd = GetFlushRecPtr();
	serverInfo.walSegSize = wal_segment_size;
	serverInfo.pgVersion = PG_VERSION_NUM;
	if (!zenith_timeline_walproposer)
		elog(FATAL, "zenith.zenith_timeline is not provided");
	if (*zenith_timeline_walproposer != '\0' &&
	 !HexDecodeString(serverInfo.ztimelineid, zenith_timeline_walproposer, 16))
		elog(FATAL, "Could not parse zenith.zenith_timeline, %s", zenith_timeline_walproposer);
	serverInfo.protocolVersion = SK_PROTOCOL_VERSION;
	pg_strong_random(&serverInfo.nodeId.uuid, sizeof(serverInfo.nodeId.uuid));
	serverInfo.systemId = GetSystemIdentifier();

	last_reconnect_attempt = GetCurrentTimestamp();

	application_name = (char*)"safekeeper_proxy"; /* for synchronous_standby_names */
	am_wal_proposer = true;
	am_walsender = true;
	InitWalSender();
	ResetWalProposerEventSet();

	/* Initiate connections to all walkeeper nodes */
	for (int i = 0; i < n_walkeepers; i++)
	{
		ResetConnection(i);
	}

	while (true)
		WalProposerPoll();
}

static void
WalProposerStartStreaming(XLogRecPtr startpos)
{
	StartReplicationCmd cmd;
	/*
	 * Always start streaming at the beginning of a segment
	 */
	startpos -= XLogSegmentOffset(startpos, serverInfo.walSegSize);

	cmd.slotname = NULL;
	cmd.timeline = serverInfo.timeline;
	cmd.startpoint = startpos;
	StartReplication(&cmd);
}

/*
 * Send message to the particular node
 */
static void
SendMessageToNode(int i, WalMessage* msg)
{
	ssize_t rc;

	/* If there is no pending message then send new one */
	if (walkeeper[i].currMsg == NULL)
	{
		/* Skip already acknowledged messages */
		while (msg != NULL && (msg->ackMask & (1 << i)) != 0)
			msg = msg->next;

		walkeeper[i].currMsg = msg;
	}
	else
		msg = walkeeper[i].currMsg;

	if (msg != NULL)
	{
		msg->req.restartLsn = restartLsn;
		msg->req.commitLsn = GetAcknowledgedByQuorumWALPosition();

		elog(LOG, "sending message with len %ld VCL=%X/%X to %d",
					msg->size - sizeof(WalKeeperRequest),
					(uint32) (msg->req.commitLsn >> 32), (uint32) msg->req.commitLsn, i);

		rc = WriteSocketAsync(walkeeper[i].sock, &msg->req, msg->size);
		if (rc < 0)
		{
			ResetConnection(i);
		}
		else if ((size_t)rc == msg->size) /* message was completely sent */
		{
			walkeeper[i].asyncOffs = 0;
			walkeeper[i].state = SS_RECV_FEEDBACK;
		}
		else
		{
			/* wait until socket is available for write */
			walkeeper[i].state = SS_SEND_WAL;
			walkeeper[i].asyncOffs = rc;
			ModifyWaitEvent(waitEvents, walkeeper[i].eventPos, WL_SOCKET_READABLE|WL_SOCKET_WRITEABLE, NULL);
		}
	}
}

/*
 * Broadcast new message to all caught-up walkeepers
 */
static void
BroadcastMessage(WalMessage* msg)
{
	for (int i = 0; i < n_walkeepers; i++)
	{
		if (walkeeper[i].state == SS_IDLE && walkeeper[i].currMsg == NULL)
		{
			SendMessageToNode(i, msg);
		}
	}
}

static WalMessage*
CreateMessage(XLogRecPtr startpos, char* data, int len)
{
	/* Create new message and append it to message queue */
	WalMessage*	msg;
	XLogRecPtr endpos;
	len -= XLOG_HDR_SIZE;
	endpos = startpos + len;
	if (msgQueueTail && msgQueueTail->req.endLsn >= endpos)
	{
		/* Message already queued */
		return NULL;
	}
	Assert(len >= 0);
	msg = (WalMessage*)malloc(sizeof(WalMessage) + len);
	if (msgQueueTail != NULL)
		msgQueueTail->next = msg;
	else
		msgQueueHead = msg;
	msgQueueTail = msg;

	msg->size = sizeof(WalKeeperRequest) + len;
	msg->next = NULL;
	msg->ackMask = 0;
	msg->req.beginLsn = startpos;
	msg->req.endLsn = endpos;
	msg->req.senderId = prop.nodeId;
	memcpy(&msg->req+1, data + XLOG_HDR_SIZE, len);

	Assert(msg->req.endLsn >= lastSentLsn);
	lastSentLsn = msg->req.endLsn;
	return msg;
}

void
WalProposerBroadcast(XLogRecPtr startpos, char* data, int len)
{
	WalMessage* msg = CreateMessage(startpos, data, len);
	if (msg != NULL)
		BroadcastMessage(msg);
}

/*
 * Create WAL message with no data, just to let the walkeepers
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

	msg = (WalMessage*)malloc(sizeof(WalMessage));
	if (msgQueueTail != NULL)
		msgQueueTail->next = msg;
	else
		msgQueueHead = msg;
	msgQueueTail = msg;

	msg->size = sizeof(WalKeeperRequest);
	msg->next = NULL;
	msg->ackMask = 0;
	msg->req.beginLsn = lastSentLsn;
	msg->req.endLsn = lastSentLsn;
	msg->req.senderId = prop.nodeId;
	/* restartLsn and commitLsn are set just before the message sent, in SendMessageToNode() */
	return msg;
}


/*
 * Prepare vote request for election
 */
static void
StartElection(void)
{
	// FIXME: If the WAL acceptors have nothing, start from "the beginning of time"
	XLogRecPtr initWALPos = serverInfo.walSegSize;
	prop.VCL = restartLsn = initWALPos;
	prop.nodeId = serverInfo.nodeId;
	for (int i = 0; i < n_walkeepers; i++)
	{
		if (walkeeper[i].state == SS_VOTING)
		{
			prop.nodeId.term = Max(walkeeper[i].info.server.nodeId.term, prop.nodeId.term);
			restartLsn = Max(walkeeper[i].info.restartLsn, restartLsn);
			if (walkeeper[i].info.epoch > prop.epoch
				|| (walkeeper[i].info.epoch == prop.epoch && walkeeper[i].info.flushLsn > prop.VCL))

			{
				prop.epoch = walkeeper[i].info.epoch;
				prop.VCL = walkeeper[i].info.flushLsn;
				leader = i;
			}
		}
	}
	/* Only walkeepers from most recent epoch can report it's FlushLsn to master */
	for (int i = 0; i < n_walkeepers; i++)
	{
		if (walkeeper[i].state == SS_VOTING)
		{
			if (walkeeper[i].info.epoch == prop.epoch)
			{
				walkeeper[i].feedback.flushLsn = walkeeper[i].info.flushLsn;
			}
			else
			{
				elog(WARNING, "WalKeeper %s:%s belongs to old epoch " INT64_FORMAT " while current epoch is " INT64_FORMAT,
					walkeeper[i].host,
					walkeeper[i].port,
					walkeeper[i].info.epoch,
					prop.epoch);
			}
		}
	}
	prop.nodeId.term += 1;
	prop.epoch += 1;
}


static void
ReconnectWalKeepers(void)
{
	/* Initiate reconnect if timeout is expired */
	TimestampTz now = GetCurrentTimestamp();
	if (wal_acceptor_reconnect_timeout > 0 && now - last_reconnect_attempt > wal_acceptor_reconnect_timeout*1000)
	{
		last_reconnect_attempt = now;
		for (int i = 0; i < n_walkeepers; i++)
		{
			if (walkeeper[i].state == SS_OFFLINE)
				ResetConnection(i);
		}
	}
}

/*
 * Receive WAL from most advanced WAL keeper
 */
static bool
WalProposerRecovery(int leader, TimeLineID timeline, XLogRecPtr startpos, XLogRecPtr endpos)
{
	char conninfo[MAXCONNINFO];
	char *err;
	WalReceiverConn *wrconn;
	WalRcvStreamOptions options;

	sprintf(conninfo, "host=%s port=%s dbname=replication",
			walkeeper[leader].host, walkeeper[leader].port);
	wrconn = walrcv_connect(conninfo, false, "wal_proposer_recovery", &err);
	if (!wrconn)
	{
		ereport(WARNING,
				(errmsg("could not connect to WAL acceptor %s:%s: %s",
						walkeeper[leader].host, walkeeper[leader].port,
						err)));
		return false;
	}
	elog(LOG, "Start recovery from %s:%s starting from %X/%08X till %X/%08X timeline %d",
		 walkeeper[leader].host, walkeeper[leader].port,
		 (uint32)(startpos>>32), (uint32)startpos, (uint32)(endpos >> 32), (uint32)endpos,
		 timeline);

	options.logical = false;
	options.startpoint = startpos;
	options.slotname = NULL;
	options.proto.physical.startpointTLI = timeline;

	if (walrcv_startstreaming(wrconn, &options))
	{
		XLogRecPtr rec_start_lsn;
		XLogRecPtr rec_end_lsn;
		int len;
		char *buf;
		pgsocket wait_fd = PGINVALID_SOCKET;
		while ((len = walrcv_receive(wrconn, &buf, &wait_fd)) > 0)
		{
			Assert(buf[0] == 'w');
			memcpy(&rec_start_lsn, &buf[XLOG_HDR_START_POS], sizeof rec_start_lsn);
			rec_start_lsn = pg_ntoh64(rec_start_lsn);
			rec_end_lsn = rec_start_lsn + len - XLOG_HDR_SIZE;
			(void)CreateMessage(rec_start_lsn, buf, len);
			if (rec_end_lsn >= endpos)
				break;
		}
		walrcv_endstreaming(wrconn, &timeline);
		walrcv_disconnect(wrconn);
	}
	else
	{
		ereport(LOG,
				(errmsg("primary server contains no more WAL on requested timeline %u LSN %X/%08X",
						timeline, (uint32)(startpos >> 32), (uint32)startpos)));
		return false;
	}
	/* Setup restart point for all walkeepers */
	for (int i = 0; i < n_walkeepers; i++)
	{
		if (walkeeper[i].state == SS_IDLE)
		{
			for (WalMessage* msg = msgQueueHead; msg != NULL; msg = msg->next)
			{
				if (msg->req.endLsn <= walkeeper[i].info.flushLsn)
				{
					msg->ackMask |= 1 << i; /* message is already received by this walkeeper */
				}
				else
				{
					SendMessageToNode(i, msg);
					break;
				}
			}
		}
	}
	return true;
}

void
WalProposerPoll(void)
{
	while (true)
	{
		WaitEvent	event;
		int rc = WaitEventSetWait(waitEvents, -1, &event, 1, WAIT_EVENT_WAL_SENDER_MAIN);
		WalKeeper*  wk = (WalKeeper*)event.user_data;
		int i = (int)(wk - walkeeper);

		/* If wait is terminated by error, postmaster die or latch event, then exit loop */
		if (rc <= 0 || (event.events & (WL_POSTMASTER_DEATH|WL_LATCH_SET)) != 0)
		{
			ResetLatch(MyLatch);
			break;
		}

		/* communication with walkeepers */
		if (event.events & WL_SOCKET_READABLE)
		{
			switch (wk->state)
			{
				case SS_HANDSHAKE:
					/* Receive walkeeper node state */
					rc = ReadSocketAsync(wk->sock,
										 (char*)&wk->info + wk->asyncOffs,
										 sizeof(wk->info) - wk->asyncOffs);
					if (rc < 0)
					{
						ResetConnection(i);
					}
					else if ((wk->asyncOffs += rc) == sizeof(wk->info))
					{
						/* WalKeeper response completely received */

						/* Check protocol version */
						if (wk->info.server.protocolVersion != SK_PROTOCOL_VERSION)
						{
							elog(WARNING, "WalKeeper has incompatible protocol version %d vs. %d",
								wk->info.server.protocolVersion, SK_PROTOCOL_VERSION);
							ResetConnection(i);
						}
						else
						{
							wk->state = SS_VOTING;
							wk->feedback.flushLsn = restartLsn;
							wk->feedback.hs.ts = 0;

							/* Check if we have quorum */
							if (++n_connected >= quorum)
							{
								if (n_connected == quorum)
									StartElection();

								/* Now send max-node-id to everyone participating in voting and wait their responses */
								for (int j = 0; j < n_walkeepers; j++)
								{
									if (walkeeper[j].state == SS_VOTING)
									{
										if (!WriteSocket(walkeeper[j].sock, &prop, sizeof(prop)))
										{
											ResetConnection(j);
										}
										else
										{
											walkeeper[j].asyncOffs = 0;
											walkeeper[j].state = SS_WAIT_VERDICT;
										}
									}
								}
							}
						}
					}
					break;

				case SS_WAIT_VERDICT:
					/* Receive walkeeper response for our candidate */
					rc = ReadSocketAsync(wk->sock,
										 (char*)&wk->info.server.nodeId + wk->asyncOffs,
										 sizeof(wk->info.server.nodeId) - wk->asyncOffs);
					if (rc < 0)
					{
						ResetConnection(i);
					}
					else if ((wk->asyncOffs += rc) == sizeof(wk->info.server.nodeId))
					{
						/* Response completely received */

						/* If server accept our candidate, then it returns it in response */
						if (CompareNodeId(&wk->info.server.nodeId, &prop.nodeId) != 0)
						{
							elog(FATAL, "WalKeeper %s:%s with term " INT64_FORMAT " rejects our connection request with term " INT64_FORMAT "",
								wk->host, wk->port,
								wk->info.server.nodeId.term, prop.nodeId.term);
						}
						else
						{
							/* Handshake completed, do we have quorum? */
							wk->state = SS_IDLE;
							if (++n_votes == quorum)
							{
								elog(LOG, "Successfully established connection with %d nodes, VCL %X/%X",
									 quorum,
									 (uint32) (prop.VCL >> 32), (uint32) (prop.VCL)
									);

								/* Check if not all safekeepers are up-to-date, we need to download WAL needed to synchronize them */
								if (restartLsn != prop.VCL)
								{
									/* Perform recovery */
									if (!WalProposerRecovery(leader, serverInfo.timeline, restartLsn, prop.VCL))
										elog(FATAL, "Failed to recover state");
								}
								WalProposerStartStreaming(prop.VCL);
								/* Should not return here */
							}
							else
							{
								/* We are already streaming WAL: send all pending messages to the attached walkeeper */
								SendMessageToNode(i, msgQueueHead);
							}
						}
					}
					break;

			    case SS_RECV_FEEDBACK:
					/* Read walkeeper response with flushed WAL position */
				    rc = ReadSocketAsync(wk->sock,
										 (char*)&wk->feedback + wk->asyncOffs,
										 sizeof(wk->feedback) - wk->asyncOffs);
					if (rc < 0)
					{
						ResetConnection(i);
					}
					else if ((wk->asyncOffs += rc) == sizeof(wk->feedback))
					{
						WalMessage* next = wk->currMsg->next;
						Assert(wk->feedback.flushLsn == wk->currMsg->req.endLsn);
						wk->currMsg->ackMask |= 1 << i; /* this walkeeper confirms receiving of this message */
						wk->state = SS_IDLE;
						wk->asyncOffs = 0;
						wk->currMsg = NULL;
						HandleWalKeeperResponse();
						SendMessageToNode(i, next);

						/*
						 * Also send the new VCL to all the walkeepers.
						 *
						 * FIXME: This is redundant for walkeepers that have other outbound messages
						 * pending.
						 */
						if (true)
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
					break;

				case SS_IDLE:
					elog(WARNING, "WalKeeper %s:%s drops connection", wk->host, wk->port);
					ResetConnection(i);
					break;

				default:
		  			elog(FATAL, "Unexpected walkeeper %s:%s read state %d", wk->host, wk->port, wk->state);
			}
		}
		else if (event.events & WL_SOCKET_WRITEABLE)
		{
			switch (wk->state)
			{
				case SS_CONNECTING:
				{
					int			optval = 0;
					ACCEPT_TYPE_ARG3 optlen = sizeof(optval);
					if (getsockopt(wk->sock, SOL_SOCKET, SO_ERROR, (char *) &optval, &optlen) < 0 || optval != 0)
					{
						elog(WARNING, "Failed to connect to node '%s:%s': %s",
							 wk->host, wk->port,
							 strerror(optval));
						closesocket(wk->sock);
						wk->sock =  PGINVALID_SOCKET;
						wk->state = SS_OFFLINE;
						ResetWalProposerEventSet();
					}
					else
					{
						uint32 len = 0;
						ModifyWaitEvent(waitEvents, wk->eventPos, WL_SOCKET_READABLE, NULL);
						/*
						 * Start handshake: send information about server.
						 * First of all send 0 as package size: it allows walkeeper to distinguish
						 * wal_proposer's connection from standard replication connection from pagers.
						 */
						if (WriteSocket(wk->sock, &len, sizeof len)
							&& WriteSocket(wk->sock, &serverInfo, sizeof serverInfo))
						{
							wk->state = SS_HANDSHAKE;
							wk->asyncOffs = 0;
						}
						else
						{
							ResetConnection(i);
						}
					}
					break;
				}

				case SS_SEND_WAL:
					rc = WriteSocketAsync(wk->sock, (char*)&wk->currMsg->req + wk->asyncOffs, wk->currMsg->size - wk->asyncOffs);
					if (rc < 0)
					{
						ResetConnection(i);
					}
					else if ((wk->asyncOffs += rc) == wk->currMsg->size)
					{
						/* WAL block completely sent */
						wk->state = SS_RECV_FEEDBACK;
						wk->asyncOffs = 0;
						ModifyWaitEvent(waitEvents, wk->eventPos, WL_SOCKET_READABLE, NULL);
					}
					break;

				default:
					elog(FATAL, "Unexpected write state %d", wk->state);
			}
		}
		ReconnectWalKeepers();
	}
}


/*
 * WalProposerRegister
 *		Register a background worker porposing WAL to wal acceptors
 */
void
WalProposerRegister(void)
{
	BackgroundWorker bgw;

	if (*wal_acceptors_list == '\0')
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "WalProposerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "WAL proposer");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "WAL proposer");
	bgw.bgw_restart_time = 5;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}
