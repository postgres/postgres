/*-------------------------------------------------------------------------
 *
 * monitor.c
 *
 * This is prototype for special monitor system.
 * The idea is that monitoring is implemented through usual backends,
 * which means there's no way to  monitor systems without looking at logs
 * (no way to connect to db cluster),
 * so this is gonna be special process that is possible to supply statistics
 * and other monitoring data even during recovery (when db data is still inconsistent)
 * So it is what it is)
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/monitor.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * TODO: add description of Monitor Subsystem LWLocks
 * to src/backend/utils/activity/wait_event_names.txt
 *
 * TODO: add restart of the monitoring subsystem process
 * in src/backend/postmaster/postmaster.c
 */


#include "postgres.h"
#include "libpq/pqsignal.h"
#include "nodes/pg_list.h"
#include "postmaster/interrupt.h"
#include "postmaster/monitor.h"
#include "storage/buf_internals.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "storage/shm_toc.h"
#include "storage/waiteventset.h"
#include "miscadmin.h"
#include "monitorsubsystem/monitor_event.h"
#include "utils/memutils.h"

// mssSharedState *MonSubSystem_SharedState = NULL;

MonSubSystem_LocalState  monSubSysLocal;

typedef struct OqtdItem
{
    MonitorMsg msg;
    int        pn;
    int        rqn;
} OqtdItem;

Size mss_subscriberInfo_size(void);
Size mss_publisherInfo_size(void);
Size mss_subjectEntity_size(void);
static bool can_deliver(OqtdItem *item, int current_pn, int last_processed_queue);
static void oqtd_insert_sorted(List **oqtd, OqtdItem *new_item);
static void read_msgs_from_channel(int qid, List **oqtd, int current_pn);
static void deliver_from_oqtd(List **oqtd, int current_pn, int last_processed_queue);
static void deliver_message_to_subscribers(MonitorMsg *msg);

Size mss_subscriberInfo_size(void)
{
	Size sz;

	sz = MAX_SUBS_NUM * sizeof(SubscriberInfo);
	return MAXALIGN(sz);
}

Size mss_publisherInfo_size(void)
{
	Size sz;

	sz = MAX_PUBS_NUM * sizeof(PublisherInfo);
	return MAXALIGN(sz);
}

Size mss_subjectEntity_size(void)
{
	Size sz;

	sz = MAX_SUBJECT_NUM * sizeof(SubjectEntity);
	return MAXALIGN(sz);
}

Size mss_monitorChannels_size(void)
{
	Size sz;

	sz = MAX_MONITOR_CHANNELS_NUM * sizeof(monitor_channel);
	return MAXALIGN(sz);
}

/*
 * the estimate of what the size should be 
 * for ChannelData is temporary
 */
Size mss_monitorChannelData_size(void)
{
	shm_toc_estimator e;
	Size sz;

	/*
	 * Estimate how much shared memory for Channel Data we need
	 *
	 * Examle from src/backend/replication/logical/applyparallelworker.c
	 * lines 338-360
	 */
	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, MAX_MONITOR_CHANNEL_DATA_SIZE);

	/* It's too much, but let it be for some time */
	shm_toc_estimate_keys(&e, MAX_MONITOR_CHANNELS_NUM);

	sz = shm_toc_estimate(&e);
	// sz = MAX_MONITOR_CHANNELS_NUM * MAX_MONITOR_CHANNEL_DATA_SIZE;
	return MAXALIGN(sz);
}

Size MonitorShmemSize(void)
{
	Size sz;

	sz = MAXALIGN(sizeof(mssSharedState));
	sz = add_size(sz, mss_subscriberInfo_size());
	sz = add_size(sz, mss_publisherInfo_size());
	sz = add_size(sz, mss_subjectEntity_size());
	sz = add_size(sz, mss_monitorChannels_size());
	sz = add_size(sz, mss_monitorChannelData_size());

	/* for hash table */
	sz = add_size(sz,
				  hash_estimate_size(MAX_SUBJECT_NUM,
									 sizeof(mssEntry)));

	return sz;
}

/*
 * MonitorShmemInit
 *		Allocate and initialize monitor subsystem related shared memory
 */
void MonitorShmemInit(void)
{
	bool found;
	char *ptr;
	HASHCTL hash_ctl;

	monSubSysLocal.MonSubSystem_SharedState = 
	// MonSubSystem_SharedState = 
	(mssSharedState *) ShmemInitStruct("Monitoring Subsystem Data",
						MonitorShmemSize(),
						&found);

	if (!found)
	{

		/* LWLocks Initialization */
		LWLockInitialize(&monSubSysLocal.MonSubSystem_SharedState->lock, LWTRANCHE_MONITOR);
		LWLockInitialize(&monSubSysLocal.MonSubSystem_SharedState->sub.lock, LWTRANCHE_MONITOR_SUBSCRIBERS);
		LWLockInitialize(&monSubSysLocal.MonSubSystem_SharedState->pub.lock, LWTRANCHE_MONITOR_PUBLISHERS);

		/* Subs initialization */
		monSubSysLocal.MonSubSystem_SharedState->sub.max_subs_num = MAX_SUBS_NUM;
		monSubSysLocal.MonSubSystem_SharedState->sub.current_subs_num = 0;

		ptr = (char *)monSubSysLocal.MonSubSystem_SharedState;
		ptr += MAXALIGN(sizeof(mssSharedState));
		monSubSysLocal.MonSubSystem_SharedState->sub.subscribers = (SubscriberInfo *)ptr;

		for (int i = 0; i < MAX_SUBS_NUM; i++)
		{
			SubscriberInfo *sub = &monSubSysLocal.MonSubSystem_SharedState->sub.subscribers[i];

			sub->proc_pid = 0; /* not used yet */
			sub->channel = NULL;

			LWLockInitialize(&sub->lock, LWTRANCHE_MONITOR_SUBSCRIBER);

			for (int j = 0; j < MAX_SUBJECT_BIT_NUM; j++)
			{
				sub->bitmap[j] = 0;
			}
		}

		/* Pubs initialization */
		ptr += mss_subscriberInfo_size();
		monSubSysLocal.MonSubSystem_SharedState->pub.publishers = (PublisherInfo *)ptr;

		monSubSysLocal.MonSubSystem_SharedState->pub.max_pubs_num = MAX_PUBS_NUM;
		monSubSysLocal.MonSubSystem_SharedState->pub.current_pubs_num = 0;

		for (int i = 0; i < MAX_PUBS_NUM; i++)
		{
			PublisherInfo *pub = &monSubSysLocal.MonSubSystem_SharedState->pub.publishers[i];
			pub->id = -1;
			pub->proc_pid = 0;
			pub->channel = NULL;
		}

		/* SubjectEntity */
		
		ptr += mss_publisherInfo_size();
		monSubSysLocal.MonSubSystem_SharedState->entitiesInfo.subjectEntities = (SubjectEntity *)ptr;

		monSubSysLocal.MonSubSystem_SharedState->entitiesInfo.next_subject_hint = 0;

		for (int i = 0; i < (MAX_SUBJECT_NUM + 63) / 64; i++)
		{
			pg_atomic_init_u64(&monSubSysLocal.MonSubSystem_SharedState->entitiesInfo.subject_used[i], 0);
		}	
		

		for (int i = 0; i < MAX_SUBJECT_NUM; i++)
		{
			SubjectEntity *subj = &monSubSysLocal.MonSubSystem_SharedState->entitiesInfo.subjectEntities[i];

			subj->_routingType = ANYCAST;

			for (int j = 0; j < MAX_SUBS_BIT_NUM; j++)
			{
				pg_atomic_init_u64(&subj->bitmap_subs[j], 0);
			}
		}

		/* Channels initialization */
		ptr += mss_subjectEntity_size();

		monSubSysLocal.MonSubSystem_SharedState->channels = (monitor_channel *)ptr;

		/* Channel Data initialization */
		ptr += mss_monitorChannels_size();

		monSubSysLocal.MonSubSystem_SharedState->channels_toc = 
			shm_toc_create(PG_MONITOR_SHM_MAGIC, ptr,
				mss_monitorChannelData_size());

		/* Hash table initialization */
		ptr += mss_monitorChannelData_size();

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(SubjectKey);
		hash_ctl.entrysize = sizeof(mssEntry);
		/* TODO: This is under question... */
		hash_ctl.hcxt = TopMemoryContext;
		/* TODO:
		 * think about using match (function) in hash_ctl
		 */

		monSubSysLocal.MonSubSystem_SharedState->mss_hash = ShmemInitHash("Monitor Subject Hash",
														   MAX_SUBJECT_NUM, /* approximate number of entries */
														   MAX_SUBJECT_NUM, /* maximum number of entries */
														   &hash_ctl,
														   HASH_ELEM | HASH_BLOBS);

		if (monSubSysLocal.MonSubSystem_SharedState->mss_hash == NULL)
		{
			elog(FATAL, "could not initialize monitor subject hash table");
		}

		elog(DEBUG1, "Monitor subsystem shared memory initialized");
	}
	else
	{
		/*
		 * TODO:
		 * checks (after restart of the process or smth like that)
		 */
		elog(DEBUG1, "Attached to existing monitor subsystem shared memory");
	}
}

/*
 * Что делать дальше:
 * инициализация oqtd 
 * функция для  вставки в oqtd 
 * (че то у меня начали возникать вопросы, все ли ок...)
 * waitEventsOnQueues
 * 
 * 
 * окей, еще раз
 * oqtd - ordered queue to deliver 
 * 		- список, в который попадают “свежие” 
 * 		СООБЩЕНИЯ для сортировки.
 * О смысле сортировки:
 * - нам пришел массив с каналами, в которых что-то есть
 * - в этих каналах есть какие-то сообщения (в канале 
 * 		может быть больше одного сообщения)
 * 		в sqm_mq вроде где-то был комментарий,
 * 		что получателю приходит сигнал о получении, когда очередь 
 * 		заполнилась на какой-то процент (надо проверить))
 * - эти сообщения надо вставлять в oqtd
 * 
 * 
 * WHAT CAN BE IMPROVED:
 * if messages are read and extracted from the channel and moved 
 * in OQTD, there is a possibility if the monitoring process
 * receives messages (extracts them from the channel), adds them to oqtd, 
 * and then dies, then these messages will simply disappear...
 * Ways to fix it (currently on mind)
 * => just read the messages from the channel, and then extract
 * (is there such a possibility?)
 * => either place oqtd in shared memory so that when the process dies, 
 * it doesn't lose these messages
 * 
 * contrib/postgres_fdw/connection.c - an example immediately with CHECK_FOR_INTERRUPTS
 * and WaitLatchOrSocket
 * 
 * src/backend/postmaster/autovacuum.c 597 - example with WaitLatch and ProcessAutoVacLauncherInterrupts
 *
 * FIXME:
 * fix signal handling, bc it doesn't stop with "pg_ctl stop"
 */
void MonitoringProcessMain(const void *startup_data, size_t startup_data_len)
{
	int current_pn = 0;
	int last_processed_queue = 0;
	List *oqtd = NIL;
    monitor_channel *channels = monSubSysLocal.MonSubSystem_SharedState->channels;	

	elog(LOG, "WORKING");
	/* for a start, there should be nothing*/
	Assert(startup_data_len == 0);

	MyBackendType = B_MONITOR_SUBSYSTEM_PROCESS;
	// here might be questions about pgstat_initialize(), ReplicationSlotInitialize, etc
	// but might not!
	AuxiliaryProcessMainCommon();

	elog(LOG, "monitoring process pid = %d", MyProcPid);

	/* signals */
	/*
	 * it's on question, actually, bc i think there would be not so much deal
	 * with config
	 * But for the start, let it be
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	/*
	 * SIGINT and SIGTERM are used for fast and smart shutdown
	 * TODO: shoud later think of SIGINT handler
	 */
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	/*
	 * Actually, I think it needs to look somehow another
	 * but it would be changed later, I'm tired now...
	 * ACTUALLY, I think it should be
	 * combination of backends and (maybe) startup
	 * bc it's gonna be a mixture of background + backend
	 *
	 */
	/*
	 * Question:
	 * тут вообще вопрос, что делать с обработчиком событий 
	 * на каналах - стоит ли это помещать в  procsignal_sigusr1_handler
	 * (и делать отдельный ProcSignalReason)
	 * или нет или потом или вообще нет?
	 */
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * тут обычно создают контекст памяти для работы
	 * так если (когда) он понадобиться, создавать здесь
	 */

	/* Unblock signals (they were blocked when the postmaster forked us) */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * тут должна быть основная логика (бесконечный цикл с логикой обработки сообщений)
	 */
	for (;;)
	{
		int ch_count = 0;
		bool setqueues[MAX_MONITOR_CHANNELS_NUM];
		// elog(LOG, "the most beatiful cycle ever!!!");
		// /* 3 sec */
		// pg_usleep(1000L * 1000L * 3L);

		memset(setqueues, 0, sizeof(setqueues));

		/*
		 * WaitLatch si enough for current uses, but
		 * in the future, if any other types of channels appear,
		 * it may be necessary to use WaitEventSetWait or WaitLatchOrSocket
		 * (if there is any type of channel that will work
		 * on sockets)
		 * 
		 * TODO:
		 * разобраться, что за wait_event_info...
		 */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, MONITOR_TIMEOUT, 0);
		/* lil question is when to reset it - mb after checking all channels? */
		ResetLatch(MyLatch);
		
		// About here should be CHECK_FOR_INTERRUPTS();

		/* 
		 * тут надо будет посмотреть, по timeout это 
		 * произошло или нет,
		 * и если да, то идти к другому шагу... аааааа
		 */

		LWLockAcquire(&monSubSysLocal.MonSubSystem_SharedState->lock, LW_EXCLUSIVE);
		for (int q = 0; q < MAX_MONITOR_CHANNELS_NUM; q++)
		{
			if (channels[q].is_there_msgs) {
				setqueues[q] = true;
				ch_count++;
			}
		}
		
		current_pn++;

		if (ch_count > 0)
		{
			for (int q = 0; q < MAX_MONITOR_CHANNELS_NUM; q++)
			{
				last_processed_queue = q;

				if (setqueues[q]) 
				{
					read_msgs_from_channel(q, &oqtd, current_pn);
				}

				deliver_from_oqtd(&oqtd, current_pn, last_processed_queue);
			}
		}
		else
		{
			last_processed_queue = MAX_MONITOR_CHANNELS_NUM;

			deliver_from_oqtd(&oqtd, current_pn, last_processed_queue);
		}
	}

}



static bool
can_deliver(OqtdItem *item, int current_pn, int last_processed_queue)
{
    return (item->pn < current_pn &&
            last_processed_queue >= item->rqn);
}

static void
read_msgs_from_channel(int qid, List **oqtd, int current_pn)
{
    monitor_channel *ch =
        &monSubSysLocal.MonSubSystem_SharedState->channels[qid];

    MonitorMsg buf;
    Size out_len;

    while (ch->ops->receive_one_msg(ch, &buf, sizeof(MonitorMsg), &out_len))
    {
        OqtdItem *item = palloc(sizeof(OqtdItem));

        memcpy(&item->msg, &buf, sizeof(MonitorMsg));
        item->pn  = current_pn;
        item->rqn = qid;

        oqtd_insert_sorted(oqtd, item);
    }
}

static void
deliver_from_oqtd(List **oqtd,
                  int current_pn,
                  int last_processed_queue)
{
    while (*oqtd != NIL)
    {
        OqtdItem *item =
            (OqtdItem *) linitial(*oqtd);

        if (!can_deliver(item, current_pn, last_processed_queue))
            break;

        deliver_message_to_subscribers(&item->msg);

        *oqtd = list_delete_first(*oqtd);

        pfree(item);
    }
}

static void
deliver_message_to_subscribers(MonitorMsg *msg)
{
    mssSharedState *state =
        monSubSysLocal.MonSubSystem_SharedState;

    bool found;
    mssEntry *entry;

    entry = hash_search(state->mss_hash,
                        &msg->key,
                        HASH_FIND,
                        &found);

    if (!found)
        return;

    SubjectEntity *entity =
        &state->entitiesInfo.subjectEntities[entry->subjectEntityId];

    for (int i = 0; i < MAX_SUBS_NUM; i++)
    {
        int word = i / 64;
        int bit  = i % 64;

        uint64 mask = UINT64CONST(1) << bit;

        if (pg_atomic_read_u64(&entity->bitmap_subs[word]) & mask)
        {
            SubscriberInfo *sub =
                &state->sub.subscribers[i];

            if (sub->channel != NULL)
            {
                sub->channel->ops->send_msg(sub->channel,
                                        msg,
                                        sizeof(MonitorMsg));
            }
        }
    }
}



/*
 * Insert OqtdItem into ordered list by ascending timestamp.
 *
 * The list head may change, so we take List **.
 */
static void
oqtd_insert_sorted(List **oqtd, OqtdItem *new_item)
{
    ListCell *lc;
    int pos = 0;

    /* If the list is empty, then just add the item */
    if (*oqtd == NIL)
    {
        *oqtd = lappend(*oqtd, new_item);
        return;
    }

    foreach(lc, *oqtd)
    {
        OqtdItem *existing = lfirst(lc);

        if (new_item->msg.ts < existing->msg.ts)
        {
			*oqtd = list_insert_nth(*oqtd, pos, new_item);
            return;
        }

		pos++;
    }

    /* If ts is the largest, add it to the end */
    *oqtd = lappend(*oqtd, new_item);
}



