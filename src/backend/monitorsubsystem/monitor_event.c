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
#include "monitorsubsystem/monitor_event.h"
#include "monitorsubsystem/monitor_channel.h"
#include "monitorsubsystem/monitor_channel_type.h"
#include "miscadmin.h"



/*
 * TODO:
 * Currently this function creates only shm_mq_monitor_channel
 * BUT it's supposed that type of monitor channel should be optional
 * (currently it's only shm_mq_monitor_channel, but number of types
 * may increase and it'd better be embedded in the code now)
 * 
 * -1 means mistake
 * 
 * TODO: 
 * Think about creating enum or errors and description to them
 */
int pg_monitor_con_connect(Size shm_mq_size)
{
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
            /* it's supposed that smbd working on it, so let's consinue*/
            continue;
        }
        /*
         * TODO: select more appropriate criteria that this 
         * SubscriberInfo is free and add additional checks
         * 
         */
        if (sub->proc_pid == 0)
        {
            mySubInfo = sub;
            sub_id = i;
            monSubSysLocal.MonSubSystem_SharedState->sub.subscribers[i].proc_pid = MyProcPid;
            
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
     * allocate memory for channel
     * 
     * QUESTION:
     * Should memory for channels be initialized using palloc
     * or during MonitorShmemInit and be in MonSubSystem_SharedState??? 
     */
    myChannel = (monitor_channel *)palloc(sizeof(monitor_channel));
    
    /*
     * TODO:
     * make an adequate channel creation depending on the type of the channel
     * 
     */
    
    /*
     * allocate memory the queue
     * 
     * QUESTION:
     * Should memory be allocated here
     * or during MonitorShmemInit and be in MonSubSystem_SharedState??? 
     */
    char *queue_memory;

    /* shm_mq_monitor_channel initialization */
    if (!channel_init(myChannel, &ShmMqChannelOps, shm_mq_size, queue_memory))
    {
        pfree(myChannel);
        LWLockRelease(&sharedSubInfo->lock);
        elog(DEBUG1, "Couldn't find a place for new sunscriber");
        return -1;
    }

    
    
    /*
     * the queue and channel must be able to be accessed
     * 
     * TODO:
     * some local structure to keep access to the queue and channel
     */
    
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

