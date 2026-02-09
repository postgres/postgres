/*-------------------------------------------------------------------------
 *
 * monitor_channel_shm_mq.h
 *	  Monitor channel based on shm_mq
 *
 * IDENTIFICATION
 *	  src/include/monitorsubsystem/monitor_channel_shm_mq.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHM_MQ_MONITOR_CHANNEL_H
#define SHM_MQ_MONITOR_CHANNEL_H
#include "postgres.h"
#include "monitorsubsystem/monitor_channel.h"
#include "storage/shm_mq.h"

typedef struct MonitorChannelConfig MonitorChannelConfig;

/* 
 * Private data for shm_mq monitor channel 
 * 
 * It's not needed at the moment, but it's apparently 
 * much easier to remove it later than to add it, 
 * so we're temporarily keeping it
 */
typedef struct ShmMqChannelData
{
	shm_mq *mq;
	/* mb bool is_sender */
} ShmMqChannelData;

typedef struct ShmMqChannelLocal
{
    shm_mq_handle *handle;
} ShmMqChannelLocal;


static bool shm_mq_channel_init(monitor_channel *ch, MonitorChannelConfig *arg);

static bool shm_mq_channel_send(monitor_channel *ch, const void *data, Size len);

static bool shm_mq_channel_receive(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);

static void shm_mq_channel_cleanup(monitor_channel *ch);

static void shm_mq_channel_attach(monitor_channel *ch, ChannelRole role);

static void (*detach)(monitor_channel *ch, void *local);

const ChannelOps ShmMqChannelOps = {
	.init = shm_mq_channel_init,
	.send = shm_mq_channel_send,
	.receive = shm_mq_channel_receive,
	.cleanup = shm_mq_channel_cleanup
};

#endif /* SHM_MQ_MONITOR_CHANNEL_H */