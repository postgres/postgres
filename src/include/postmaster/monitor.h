/*-------------------------------------------------------------------------
 *
 * monitor.h
 * 	Exports from postmaster/monitor.c.
 *
 * This is the header file for new auxiliary process for monitoring needs.
 * INTERNAL HEADER.
 * Not for use by external modules or user-facing API.
 *
 * Contains:
 *  - internal data structures
 *  - monitoring auxiliary process entry point
 * 
 * Current limits:
 * Each process can have only 1 channel for subscriptions and 1 channel for publishing
 *
 * src/include/postmaster/monitor.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef _MONITOR_H
#define _MONITOR_H

#include "postmaster/auxprocess.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/shm_toc.h"
#include "monitorsubsystem/monitor_channel.h"

/*
 * NOTE: MSS_MAX_PROCESSES cannot use actual GUC variables
 * (MaxBackends, max_worker_processes, etc.) because they are
 * runtime parameters, not compile-time constants.
 *
 * Current workaround: Static conservative limits.
 *
 * FIXME / QUESTION / TODO:
 * Consider dynamic data structures instead of bitmasks
 * to avoid hard limits.
 */

/* #define MSS_MAX_PROCESSES                 \
 	(MaxBackends + max_worker_processes + \
	 autovacuum_max_workers + max_parallel_workers + 1)
*/

// guc pgc postmaster
#define MAX_BACKENDS_LIMIT 256
#define MAX_WORKER_PROCESSES_LIMIT 64
#define AUTOVACUUM_MAX_WORKERS_LIMIT 16
#define MAX_PARALLEL_WORKERS_LIMIT 64

#define MSS_MAX_PROCESSES (MAX_BACKENDS_LIMIT + MAX_WORKER_PROCESSES_LIMIT + \
						   AUTOVACUUM_MAX_WORKERS_LIMIT + MAX_PARALLEL_WORKERS_LIMIT + 1)

#define MAX_SUBS_NUM MSS_MAX_PROCESSES
#define MAX_PUBS_NUM 32
#define MAX_MONITOR_CHANNELS_NUM (MAX_PUBS_NUM + MAX_SUBS_NUM)
#define MAX_SUBJECT_NUM 64

#define MAX_SUBS_BIT_NUM ((MAX_SUBS_NUM + 64 - 1) / 64)

#define MAX_MONITOR_CHANNEL_DATA_SIZE 1024
#define PG_MONITOR_SHM_MAGIC 0x8d7c6a5b

#define MAX_SUBJECT_LEN 25
#define MAX_SUBJECT_BIT_NUM (MAX_SUBS_NUM + 64 - 1) / 64

#define MAX_MONITOR_MESSAGE_LEN 64
#define MONITOR_TIMEOUT 300

typedef enum
{
	ANYCAST,
	MULTICAST,
} routing_type;

typedef struct _subjectEnity
{
	routing_type _routingType;

	// пусть подписчики будут битовой маской
	pg_atomic_uint64 bitmap_subs[MAX_SUBS_BIT_NUM];

	// // или так, я пока не решила
	// LWLock lock;
	// uint64 bitmap[MAX_SUBS_BIT_NUM];
} SubjectEntity;

typedef struct _subjectKey
{
	char name[MAX_SUBJECT_LEN];
} SubjectKey;

typedef struct MonitorMsg
{
	SubjectKey key;
    TimestampTz ts;
    Size        len;
    char        data[MAX_MONITOR_MESSAGE_LEN];
} MonitorMsg;

/*
 * key -> SubjectEntity hash entry
 */
typedef struct mssEntry
{
	SubjectKey key;		 /* hash key */
	int subjectEntityId; /* id в массиве с SubectEntity */
						 /* возможно нужна лочка */
	/* Maybe LWLock needed */

} mssEntry;

/*
 * Для быстрой отписки (чтобы не итерировать по всем записям в хеше)
 * подписчик должен хранить информацию о том, на что он подписан
 * Это должно занимать поменьше памяти (по возможности)
 * Поэтому есть 2 вариант
 * 1.
 * сделать массив SubjectEntity вне хеш мапы, в хеш мапе сделать offset/id of SubjectEntity
 * А в SubscriberInfo хранить что-то типа битового массива на SubjectEntity
 * Однако чтобы использовать что-то типа битового массива / битовой маске, нужно
 * заранее знать максимальное число записей
 *
 * 2.
 * хранить SubjectEntity прямо в хеш-мапе, а в SubsctiberInfo хранить какой-нибудь offset
 * HTAB (dynahash) НЕ переставляет записи
 * НО тогда в SubscriberInfo придется хранить что-то вроде массива из offset'ов
 * (в хеш-мапе нет никаких "индексов" - поэтому сделать битовый массив как в 1 варианте нельзя)
 *
 * ВАРИАНТ 1 WINS
 *
 */

/* 
 * ALWAYS first local->MonSubSystem_SharedState->lock
 * then SubjectInfo->lock
 */
typedef struct SubscriberInfo
{
	int id;
	pid_t proc_pid; 
	/* тут микро вопрос, как это норм задавать - возможно, лучше не через указатели, а через offset и тд... */
	monitor_channel *channel;

	LWLock lock;
	uint64 bitmap[MAX_SUBJECT_BIT_NUM]; /* битовый массив subjectId, на которые подписчан подписчик*/
} SubscriberInfo;

typedef struct PublisherInfo
{
	int id;
	/* мб еще лочку надо добавить */
	pid_t proc_pid;
	monitor_channel *channel;
} PublisherInfo;

typedef struct MssState_SubscriberInfo
{
	LWLock lock;
	SubscriberInfo *subscribers;

	/* just in case 16, 8 might be enough */
	uint16 max_subs_num;
	uint16 current_subs_num;

} MssState_SubscriberInfo;

typedef struct MssState_PublisherInfo
{
	LWLock lock;
	PublisherInfo *publishers;

	/* just in case 16, 8 might be enough */
	uint16 max_pubs_num;
	uint16 current_pubs_num;

} MssState_PublisherInfo;

typedef struct MssState_SubjectEntitiesInfo
{
	SubjectEntity *subjectEntities;
	int next_subject_hint;
	
    pg_atomic_uint64 subject_used[(MAX_SUBJECT_NUM + 63) / 64];
} MssState_SubjectEntitiesInfo;


/*
 * еще раз - в разделяемой памяти лежит
 * структура (пока массив) со списком подписчиков
 * массив subjectEntities
 * хеш-мапа с соотношением subject-SubjectEntities
 * структура, где должны регистрироваться издатели
 *
 * к этим структурам должен быть доступ функциям подписаться-отписаться и тд
 * поэтому обернем все это в общую структуру с этим всем...
 *
 * возможно хеш-таблицу можно было бы и вынести отдельно, типа
 * static HTAB *mss_hash = NULL;
 * но я пока не решила
 */

/*
 * Central shared memory entry for the monitor subsystem
 *
 * SubsribersInfo, Publishers, subject-subscribers (SubjectEntities) hashtable
 * are reached from here.
 *
 * 
 * max_num of all monitor channels = (MAX_PUBS_NUM + MAX_SUBS_NUM)
 * so pub_num is number of publisher in PublisherInfo *publishers
 * sub_number is number of pusblisher in SubscriberInfo *subscribers;
 * so monitor_channel[i]  = { i if it's publisher's channel and i is pub_number of the puslisher,
 * i + MAX_PUBS_NUM if it's subscriber's channel and i is sub_number of the subscriber}
 */
typedef struct mssSharedState
{
	MssState_SubscriberInfo sub;
	MssState_PublisherInfo pub;
	monitor_channel *channels;
	MssState_SubjectEntitiesInfo entitiesInfo;
	shm_toc *channels_toc;


	LWLock lock;	/* protects hashtable search/modification */
	HTAB *mss_hash; /* hashtable for SubjectKey - SubjectEntity */

} mssSharedState;

// extern mssSharedState *MonSubSystem_SharedState;

typedef struct MonSubSystem_LocalState
{
	mssSharedState *MonSubSystem_SharedState;
	
	SubscriberInfo *mySubInfo;
	PublisherInfo *myPubInfo;

	/* 
	 * That's needed for some local data 
	 * like shm_mq_handle
	 */
	void *subLocalData;
	void *pubLocalData;
	/*
	 * maybe it would be better to make smth like
	 * but Idk
	 */
	// monitor_channel *monitor_sub_channel;
	// monitor_channel *monitor_pub_channel;
} MonSubSystem_LocalState;

/* Backend-local access point to Monitor SubSystem */
/* Is PGDLLIMPORT needed? */
extern PGDLLIMPORT MonSubSystem_LocalState monSubSysLocal;

// I take an example from walwriter (src/backend/postmaster/walwriter.c) and other backgrounds
pg_noreturn extern void MonitoringProcessMain(const void *startup_data, size_t startup_data_len);

/*
 * this needed to be included to CalculateShmemSize in src\backend\storage\ipc\ipci.c
 */
extern Size MonitorShmemSize(void);

/*
 * this is for initializing shmem for monitoring subsystem
 * this needed to be included to CreateOrAttachShmemStructs in src\backend\storage\ipc\ipci.c
 */
extern void MonitorShmemInit(void);

#endif /* _MONITOR_H */
