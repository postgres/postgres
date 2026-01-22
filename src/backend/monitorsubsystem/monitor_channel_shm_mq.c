/*-------------------------------------------------------------------------
 *
 * monitor_channel_shm_mq.c
 *	  Implementation of monitor channel api, based on shm_mq
 *
 * IDENTIFICATION
 *	  src/backend/monitorsubsystem/monitor_channel_shm_mq.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "monitorsubsystem/monitor_channel.h"
#include "storage/shm_mq.h"


typedef struct monitor_channel monitor_channel;

typedef struct
{
	/* data */
} ShmMqChannel;


typedef struct ChannelOps
{
	bool (*init)(monitor_channel *ch, Size size, void *arg);
	bool (*send)(monitor_channel *ch, const void *data, Size len);
	bool (*receive)(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);
	void (*cleanup)(monitor_channel *ch);
} ChannelOps;


/* не уверенна, что тут нужен этот static, но пусть пока будет */
static  bool
shm_mq_channel_init(monitor_channel *ch, const ChannelOps *ops,
			 Size *size, void *arg)
{

}

static bool
shm_mq_channel_send(monitor_channel *ch, const void *data, Size len)
{

}

static bool
shm_mq_channel_receive(monitor_channel *ch, void *buf, Size buf_size, Size *out_len)
{
	return ch->ops->receive(ch, buf, buf_size, out_len);
}

static void
shm_mq_channel_cleanup(monitor_channel *ch)
{
	return ch->ops->cleanup(ch);
}
