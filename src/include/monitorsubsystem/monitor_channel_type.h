/*-------------------------------------------------------------------------
 *
 * monitor_channel_type.h
 *	  All types of monitor channel are labeled here
 *
 * IDENTIFICATION
 *	  src/include/monitorsubsystem/monitor_channel_type.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONITOR_CHANNEL_TYPE
#define MONITOR_CHANNEL_TYPE
// #include "postgres.h"
#include "monitorsubsystem/monitor_channel.h"
#include "monitorsubsystem/monitor_channel_shm_mq.h"

typedef enum
{
    MONITOR_CHANNEL_SHM_MQ,
} MonitorChannelType;


static ChannelOps monitor_channel_options[] = {
    [MONITOR_CHANNEL_SHM_MQ] = ShmMqChannelOps,
};



#endif /* MONITOR_CHANNEL_TYPE */
