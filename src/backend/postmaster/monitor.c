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

#include "postgres.h"
#include "libpq/pqsignal.h"
#include "postmaster/interrupt.h"
#include "monitorsubsystem/monitor_event.h"

Size mss_subscriberInfo_size()
{
	Size sz;

	sz = MAX_SUBS_NUM * sizeof(SubscriberInfo);
	return MAXALIGN(sz);
}

Size mss_publisherInfo_size()
{
	Size sz;

	sz = MAX_PUBS_NUM * sizeof(PublisherInfo);
	return MAXALIGN(sz);
}

Size mss_subjectEntity_size()
{
	Size sz;

	sz = MAX_SUBJECT_NUM * sizeof(SubjectEntity);
	return MAXALIGN(sz);
}

Size MonitorShmemSize(void)
{
	Size sz;

	sz = MAXALIGN(sizeof(MssState_SubscriberInfo));
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

	/*
	 * TODO: Initialize monitoring structures
	 *
	 * what happens here
	 * - monitor subsystem memory allocation
	 * - initialyzation of all monitor subsystem entries
	 *
	 */
}

void MonitoringProcessMain(char *startup_data, size_t startup_data_len)
{
	/* for a start, there should be nothing*/
	Assert(startup_data_len == 0);

	MyBackendType = B_MONITORING;
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
