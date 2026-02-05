/*-------------------------------------------------------------------------
 *
 * monitor_event.c
 *	  API for using the Monitoring Subsystem
 *
 * IDENTIFICATION
 *	  src/backend/monitorsubsystem/monitor_event.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "monitorsubsystem/monitor_channel.h"
#include "monitorsubsystem/monitor_channel_type.h"
#include "monitorsubsystem/monitor_event.h"
#include "miscadmin.h"

#define BIT_WORD(idx) ((idx) / 64)
#define BIT_MASK(idx) (1ULL << ((idx) % 64))


/*
 * -1 means mistake
 * 
 * TODO: 
 * Think about creating enum or errors and description to them
 */
int pg_monitor_con_connect(MonitorChannelConfig *conConfig)
{
    /*
     * тут мы передаем конфиш (в нем тип канала и нужные опции)
     * после этого
     * 1 ищем место в массиве подписчиков и там регаемся
     * 2 создаем сам канал
     * 3 ставим в массиве подписчиков указатель на канал
     * 
     * что про канал 
     * - он должен располагаться в разделяемой памяти
     * (и все, что к нему относится)
     * 
     * 
     * для того, чтобы создавать КАНАЛ чисто по конфигу (то есть по типу и параметрам)
     * и НЕ мучиться с switch-case, нужен массив структур
     * в этом массиве структур по типу канала будет выдаваться все, что надо для создания этого канала
     * 
     * 
     */ 
    /* find a place for subscriber */
    int sub_id = -1;
	SubscriberInfo *mySubInfo;
    monitor_channel *myChannel;
    MssState_SubscriberInfo *sharedSubInfo = &monSubSysLocal.MonSubSystem_SharedState->sub;
    
    LWLockAcquire(&sharedSubInfo->lock, LW_EXCLUSIVE);
    
    if (sharedSubInfo->current_subs_num  == sharedSubInfo->max_subs_num)
    {
        elog(DEBUG1, "Maximum of supported subscribers is reached, a place for new pub couldn't be allocated");
        return -1;
    }

    for (int i = 0; i < sharedSubInfo->max_subs_num; i++)
    {
        SubscriberInfo *sub = &sharedSubInfo->subscribers[i];
        bool res = LWLockConditionalAcquire(&sub->lock, LW_EXCLUSIVE);
        if (!res) {
            /* it's supposed that smbd working on it, so let's continue*/
            continue;
        }
        /*
         * TODO: select more appropriate criteria that this 
         * SubscriberInfo is free and add additional checks
         * 
         */
        if (sub->id == -1) {
            mySubInfo = sub;
            sub_id = i;
            mySubInfo->proc_pid = MyProcPid;
            mySubInfo->id = sub_id;
            
            LWLockRelease(&sub->lock);
            break;
        }
        LWLockRelease(&sub->lock);
    }
    
    if (sub_id == -1)
    {
        LWLockRelease(&sharedSubInfo->lock);

        return -1;
    }
    
    /*
     * TODO:
     * make an adequate channel creation depending on the type of the channel
     * 
     */

    /*
     * allocate memory for the monitor channel
     * 
     * QUESTION:
     * Should memory be allocated here
     * or during MonitorShmemInit and be in MonSubSystem_SharedState??? 
     */
    /*
     * в чем вообще проблема
     * 1 где выделять память под каналы? с учетом того, что они ДОЛЖНЫ
     * быть доступны процессу мониторинга (= лежать в разделяемой памяти)
     * при этом как будто все структуры, которые касаются этих каналов, ТОЖЕ
     * должны быть доступны процессу мониторинга
     * (ну вот есть канал, а еще у него есть такая штука, как void *private_data, где он,
     * по моему мнению, должен хранить всякую доп инфу о себе, В ЗАВИСИМОСТИ от типа своего канала
     * (Например, для shm_mq_monitor_channel там должны быть всякие shm_mq* и shm_mq_handle*, 
     * вот такого рода штуки))
     * 
     * Окей, кому вообще нужны эти каналы?
     * Они нужны процессу мониторинга и получетелям канала / отправителям канала
     * 
     * Как сейчас доступаются к этому каналу?
     * есть ссылки в SubscriberInfo и в PublisherInfo на каналы, но при этом эти каналы
     * создаются Бог пойми где
     * вот сейчас (пока что) есть эта "плохая" строчка 
     * myChannel = (monitor_channel *)palloc(sizeof(monitor_channel));
     * 
     * 
     */
    myChannel = monSubSysLocal.MonSubSystem_SharedState->channels[sub_id + MAX_PUBS_NUM];
    

    
    bool is_channel_created = monitor_channel_options[conConfig->type].init(myChannel, conConfig);

    if (! is_channel_created) {
        LWLockRelease(&sharedSubInfo->lock);
        elog(DEBUG1, "Couldn't create a channel");
        return -1 ;
    }

    /*
     * maybe it'd be easier just not to release lock 
     * immideatly after finding mySubInfo
     *
     * 
     */
    LWLockAcquire(&mySubInfo->lock, LW_EXCLUSIVE);

    mySubInfo->channel = myChannel;
    
    sharedSubInfo->current_subs_num++;
    monSubSysLocal.mySubInfo = mySubInfo;


    LWLockRelease(&mySubInfo->lock);
    LWLockRelease(&sharedSubInfo->lock);
    
    return 0;
}


/*
 * -1 means mistake
 * 
 * TODO: 
 * Think about creating enum or errors and description to them
 */
int pg_monitor_pub_connect(MonitorChannelConfig *conConfig)
{
    /* find a place for publisher */
    int pub_id = -1;
	PublisherInfo *myPubInfo;
    monitor_channel *myChannel;
    MssState_PublisherInfo *sharedPubInfo = &monSubSysLocal.MonSubSystem_SharedState->pub;
    
    LWLockAcquire(&sharedPubInfo->lock, LW_EXCLUSIVE);
    
    if (sharedPubInfo->max_pubs_num  == sharedPubInfo->current_pubs_num)
    {
        elog(DEBUG1, "Maximum of supported publishers is reached, a place for new pub couldn't be allocated");
        return -1;
    }

    for (int i = 0; i < sharedPubInfo->max_pubs_num; i++)
    {
        PublisherInfo *pub = &sharedPubInfo->publishers[i];
        bool res = LWLockConditionalAcquire(&pub->lock, LW_EXCLUSIVE);
        if (!res) {
            /* it's supposed that smbd working on it, so let's continue*/
            continue;
        }
        /*
         * TODO: select more appropriate criteria that this 
         * PublisherInfo is free and add additional checks
         */
        if (pub->id == -1) {
            myPubInfo = pub;
            pub_id = i;
            myPubInfo->proc_pid = MyProcPid;
            myPubInfo->id = pub_id;
            
            LWLockRelease(&pub->lock);
            break;
        }
        LWLockRelease(&pub->lock);
    }
    
    if (pub_id == -1)
    {
        LWLockRelease(&sharedPubInfo->lock);

        return -1;
    }
    
    /*
     * TODO:
     * allocate memory for the monitor channel
     */
    myChannel = monSubSysLocal.MonSubSystem_SharedState->channels[pub_id];

    
    bool is_channel_created = monitor_channel_options[conConfig->type].init(myChannel, conConfig);

    if (! is_channel_created) {
        LWLockRelease(&sharedPubInfo->lock);
        elog(DEBUG1, "Couldn't create a channel");
        return -1 ;
    }

    /*
     * maybe it'd be easier just not to release lock 
     * immideatly after finding myPubInfo
     */
    LWLockAcquire(&myPubInfo->lock, LW_EXCLUSIVE);

    myPubInfo->channel = myChannel;
    
    sharedPubInfo->current_pubs_num++;
    monSubSysLocal.myPubInfo = myPubInfo;


    LWLockRelease(&myPubInfo->lock);
    LWLockRelease(&sharedPubInfo->lock);
    
    return 0;
}


int pg_monitor_subscribe_to_event(const char *event_string, routing_type _routing_type)
{
    /*
     * Что надо сделать чтобы подписаться на событие
     * найти событие в хэш-таблице
     * если нету - создать и создать новую записть в subjectEntities
     * поставить в SubjectEntity битик о номере SubscriberInfo
     * потом поставить в битовом массиве SubscriberInfo инфу бит с номером SubjectEntities
     * 
     * 
     * что для этого надо в параметрах:
     * - SubscriberInfo* - но оно будет в MonSubSystem_LocalState
     * (сейчас есть ограничение, что 1 канал для записи и один для публикации сообщений)
     * - routingType (если события еще нет, надо задать ключ)
     * 
     * проверки
     * - что event_string проходит по лимиту (MAX_SUBJECT_LEN)
     * - что подписчик зарегестрирован
     * - если запись уже существует - то надо проверить, что routing_type совпадает
     * 
     * ошибки
     * - подписчик не зарегестрирован
     * - event_string слишком большое
     * - routing_type не совпадает
     * 
     */

    MonSubSystem_LocalState *local = &monSubSysLocal;
    mssSharedState *shared;
    SubscriberInfo *sub;
    SubjectEntity *subject;
    mssEntry *entry;
    bool found;
    SubjectKey key;
    int subjectId;

    if (local->mySubInfo == NULL)
        return -1; /* subscriber not registered */

    if (event_string == NULL)
        return -1;

    if (strlen(event_string) >= MAX_SUBJECT_LEN)
        return -1;

    sub = local->mySubInfo;
    shared = local->MonSubSystem_SharedState;


    memset(&key, 0, sizeof(key));
    strlcpy(key.name, event_string, MAX_SUBJECT_LEN);

    LWLockAcquire(&shared->lock, LW_EXCLUSIVE);

    /* specify HASHCATION */
    entry = hash_search(shared->mss_hash,
                        (void *) &key,
                        HASH_ENTER,
                        &found);

    if (!found)
    {
        /*
         * TODO:
         * update with MssState_SubjectEntitiesInfo.subject_used
         */
        subjectId = -1;

        for (int i = 0; i < MAX_SUBJECT_NUM; i++)
        {
            SubjectEntity *cand = &shared->subjectEntities[i];

            bool empty = true;
            for (int w = 0; w < MAX_SUBS_BIT_NUM; w++)
            {
                if (pg_atomic_read_u64(&cand->bitmap_subs[w]) != 0)
                {
                    empty = false;
                    break;
                }
            }

            if (empty)
            {
                subjectId = i;
                break;
            }
        }

        if (subjectId == -1)
        {
            LWLockRelease(&shared->lock);
            return -1; /* no free subject slots */
        }

        subject = &shared->subjectEntities[subjectId];

        subject->_routingType = routing;
        for (int w = 0; w < MAX_SUBS_BIT_NUM; w++)
            pg_atomic_write_u64(&subject->bitmap_subs[w], 0);

        entry->subjectEntityId = subjectId;
    }
    else
    {
        subjectId = entry->subjectEntityId;
        subject = &shared->subjectEntities[subjectId];

        if (subject->_routingType != routing)
        {
            LWLockRelease(&shared->lock);
            return -1; /* routing type mismatch */
        }
    }

    /* update SubjectEntity bitmap */

    int subId = sub->id;
    int word = BIT_WORD(subId);
    uint64 mask = BIT_MASK(subId);

    pg_atomic_fetch_or_u64(&subject->bitmap_subs[word], mask);

    /* update SubscriberInfo bitmap */

    LWLockAcquire(&sub->lock, LW_EXCLUSIVE);

    int subj_word = BIT_WORD(subjectId);
    uint64 subj_mask = BIT_MASK(subjectId);

    sub->bitmap[subj_word] |= subj_mask;

    LWLockRelease(&sub->lock);
    LWLockRelease(&shared->lock);

    return 0;


}

