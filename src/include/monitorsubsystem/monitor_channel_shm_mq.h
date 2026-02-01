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

/* Private data for shm_mq monitor channel */
typedef struct ShmMqChannelData
{
	shm_mq_handle *mq_handle;
	shm_mq *mq;
	Size size;
	/* mb bool is_sender */
} ShmMqChannelData;

static bool shm_mq_channel_init(monitor_channel *ch, const ChannelOps *ops,
					Size *size, void *arg);

static bool shm_mq_channel_send(monitor_channel *ch, const void *data, Size len);

static bool shm_mq_channel_receive(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);

static void shm_mq_channel_cleanup(monitor_channel *ch);

const ChannelOps ShmMqChannelOps = {
	.init = shm_mq_channel_init,
	.send = shm_mq_channel_send,
	.receive = shm_mq_channel_receive,
	.cleanup = shm_mq_channel_cleanup
};

#endif /* SHM_MQ_MONITOR_CHANNEL_H */