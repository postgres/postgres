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
#include "postmaster/interrupt.h"
#include "postmaster/monitor.h"
#include "storage/buf_internals.h"
#include "monitorsubsystem/monitor_event.h"
#include "utils/memutils.h"

// mssSharedState *MonSubSystem_SharedState = NULL;

MonSubSystem_LocalState  monSubSysLocal;

Size mss_subscriberInfo_size(void);
Size mss_publisherInfo_size(void);
Size mss_subjectEntity_size(void);

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

Size MonitorShmemSize(void)
{
	Size sz;

	sz = MAXALIGN(sizeof(mssSharedState));
	sz = add_size(sz, mss_subscriberInfo_size());
	sz = add_size(sz, mss_publisherInfo_size());
	sz = add_size(sz, mss_subjectEntity_size());

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
			pub->proc_pid = 0;
			pub->sub_channel = NULL;
		}

		/* SubjectEntity array initialization */
		ptr += mss_publisherInfo_size();
		monSubSysLocal.MonSubSystem_SharedState->subjectEntities = (SubjectEntity *)ptr;

		for (int i = 0; i < MAX_SUBJECT_NUM; i++)
		{
			SubjectEntity *subj = &monSubSysLocal.MonSubSystem_SharedState->subjectEntities[i];

			subj->_routingType = ANYCAST;

			for (int j = 0; j < MAX_SUBS_BIT_NUM; j++)
			{
				pg_atomic_init_u64(&subj->bitmap_subs[j], 0);
			}
		}

		/* Hash table initialization */
		ptr += mss_subjectEntity_size();

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
 * FIXME:
 * fix signal handling, bc it doesn't stop with "pg_ctl stop"
 */
void MonitoringProcessMain(const void *startup_data, size_t startup_data_len)
{

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
	 * TODO: доделать sigusr1 handler
	 * (shm_mq использует latch, а latch использует SIGUSR1)
	 */
	pqsignal(SIGUSR1, SIG_IGN);
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
		elog(LOG, "the most beatiful cycle ever!!!");
		/* 3 sec */
		pg_usleep(1000L * 1000L * 3L);
	}
}
