/*-------------------------------------------------------------------------
 *
 * monitor_channel.h
 *	  Api of monitor channel, used in monitor Subsystem to deliver messages to consumers
 *
 * Channels for monitoring susbsystem must be created by publishers
 * and subscribers-processes, not by monitor process
 * 
 * IDENTIFICATION
 *	  src/include/monitorsubsystem/monitor_channel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONITOR_CHANNEL
#define MONITOR_CHANNEL
#include "postgres.h"

#define CH_ATTACH_ACTIVE \
    (CH_ATTACH_CLIENT | CH_ATTACH_MONITOR)

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

typedef enum
{
    CH_UNUSED = 0,        
    CH_CREATED,           /* channel created by a client*/
	CH_ACTIVE,             /* channel is ready to use */
    CH_CLOSED
} ChannelState;

typedef enum
{
    CH_ATTACH_NONE      = 0,
    CH_ATTACH_CLIENT    = 1 << 0,
    CH_ATTACH_MONITOR  = 1 << 1
} ChannelAttachFlags;


typedef struct ChannelOps
{
	bool (*init)(monitor_channel *ch, MonitorChannelConfig *arg);
	bool (*send_msg)(monitor_channel *ch, const void *data, Size len);
	ChannelRecvResult (*receive_one_msg)(monitor_channel *ch, void *buf, Size buf_size, Size *out_len);
	void (*cleanup)(monitor_channel *ch);
	void *(*attach)(monitor_channel *ch);
    void (*detach)(monitor_channel *ch, void *local);

} ChannelOps;

typedef struct monitor_channel 
{
	const ChannelOps *ops;
	/* private implementation data (mb needed) */
    void *private_data;
	/* temporary */
	bool is_there_msgs; 
	int publisher_procno;
    int subscriber_procno;

	ChannelState state;
    uint8 attach_flags;
	
	slock_t mutex;

} monitor_channel;

static inline bool
channel_is_ready(uint8 flags)
{
    return (flags & (CH_ATTACH_CLIENT | CH_ATTACH_MONITOR))
           == (CH_ATTACH_CLIENT | CH_ATTACH_MONITOR);
};

#endif /* MONITOR_CHANNEL */
