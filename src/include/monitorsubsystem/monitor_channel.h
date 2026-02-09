/*-------------------------------------------------------------------------
 *
 * monitor_channel.h
 *	  Api of monitor channel, used in monitor Subsystem to deliver messages to consumers
 *
 * IDENTIFICATION
 *	  src/include/monitorsubsystem/monitor_channel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONITOR_CHANNEL
#define MONITOR_CHANNEL
#include "postgres.h"

// struct monitor_channel;
typedef struct monitor_channel monitor_channel;
typedef struct MonitorChannelConfig MonitorChannelConfig;

/*
 * тк сейчас используем чисто shm_mq, но в будущем могут быть добавлены
 * и другие реализации, то имеет смысл сейчас (даже на всякий случай)
 * создать интерфейс для канала и, если что, добавлять реализации
 * по мере нужды
 *
 */

typedef enum
{
	Publisher,
	Subscriber
} ChannelRole;

typedef struct ChannelOps
{
	bool (*init)(monitor_channel *ch, MonitorChannelConfig *arg);
	bool (*send)(monitor_channel *ch, const void *data, Size len);
	bool (*receive)(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);
	void (*cleanup)(monitor_channel *ch);
	void *(*attach)(monitor_channel *ch, ChannelRole role);
    void (*detach)(monitor_channel *ch, void *local);

} ChannelOps;

typedef struct monitor_channel 
{
	const ChannelOps *ops;
	/* private implementation data (mb needed) */
    void *private_data;    

} monitor_channel;


#endif /* MONITOR_CHANNEL */
