/*-------------------------------------------------------------------------
 *
 * monitor_channel.c
 *	  Api of monitor channel, used in monitor Subsystem to deliver messages to consumers
 *
 * IDENTIFICATION
 *	  src/include/monitorsubsystem/monitor_channel.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONITOR_CHANNEL
#define MONITOR_CHANNEL
#include "postgres.h"

// struct monitor_channel;
typedef struct monitor_channel monitor_channel;

/*
 * тк сейчас используем чисто shm_mq, но в будущем могут быть добавлены
 * и другие реализации, то имеет смысл сейчас (даже на всякий случай)
 * создать интерфейс для канала и, если что, добавлять реализации
 * по мере нужды
 *
 */

typedef struct ChannelOps
{
	bool (*init)(monitor_channel *ch, Size size, void *arg);
	bool (*send)(monitor_channel *ch, const void *data, Size len);
	bool (*receive)(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);
	void (*cleanup)(monitor_channel *ch);

} ChannelOps;

struct monitor_channel
{
	const ChannelOps *ops;
	void *impl;

};

static inline bool
channel_init(monitor_channel *ch, const ChannelOps *ops,
			 Size *size, void *arg)
{
	ch->ops = ops;
	return ops->init(ch, size, arg);
}

static inline bool
channel_send(monitor_channel *ch, const void *data, Size len)
{
	return ch->ops->send(ch, data, len);
}

static inline bool
channel_receive(monitor_channel *ch, void *buf, Size buf_size, Size *out_len)
{
	return ch->ops->receive(ch, buf, buf_size, out_len);
}

static inline void
channel_cleanup(monitor_channel *ch)
{
	return ch->ops->cleanup(ch);
}

#endif /* MONITOR_CHANNEL */
