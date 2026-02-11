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

/* 
 * QUESTION:
 * does it make any sense? 
 */
typedef enum
{
    CHANNEL_RECV_OK,
    CHANNEL_RECV_EMPTY,
    CHANNEL_RECV_CLOSED
} ChannelRecvResult;


typedef struct ChannelOps
{
	bool (*init)(monitor_channel *ch, MonitorChannelConfig *arg);
	bool (*send_msg)(monitor_channel *ch, const void *data, Size len);
	ChannelRecvResult (*receive_one_msg)(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);
	void (*cleanup)(monitor_channel *ch);
	void *(*attach)(monitor_channel *ch, ChannelRole role);
    void (*detach)(monitor_channel *ch, void *local);

} ChannelOps;

typedef struct monitor_channel 
{
	const ChannelOps *ops;
	/* private implementation data (mb needed) */
    void *private_data;
	/* temporary */
	bool is_there_msgs; 
	
	/* mb LWLock needed? */

} monitor_channel;


#endif /* MONITOR_CHANNEL */
