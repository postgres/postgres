/*-------------------------------------------------------------------------
 *
 * monitor_channel_shm_mq.c
 *	  Implementation of monitor channel api based on shm_mq
 *
 * IDENTIFICATION
 *	  src/backend/monitorsubsystem/monitor_channel_shm_mq.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "monitorsubsystem/monitor_channel.h"
#include "monitorsubsystem/monitor_channel_type.h"
#include "monitorsubsystem/monitor_channel_shm_mq.h"
#include "storage/shm_mq.h"


/* static needed ? */
/*
 * тут в arg надо будет передать всю херню, которая будет нужна для данного типа канала
 *
 */
static bool
shm_mq_channel_init(monitor_channel *ch, MonitorChannelConfig *arg)
{
	/*
	 * TODO:
	 * realize
	 * 
	 */
	// char *memory = (char *)arg;

	// ShmMqChannelData *priv = (ShmMqChannelData *)palloc(sizeof(ShmMqChannelData));
	// if (!priv)
	// 	return false;

	// priv->mq = shm_mq_create(memory, size);
	// priv->size = size;
	// priv->mq_handle = shm_mq_attach(priv->mq, NULL, NULL);

	// ch->private_data = priv;
	// return true;
	return true;
}

static bool
shm_mq_channel_send(monitor_channel *ch, const void *data, Size len)
{
	ShmMqChannelData *priv = (ShmMqChannelData *)ch->private_data;

	return shm_mq_send(priv->mq_handle, len, data, false, false);
}

static bool
shm_mq_channel_receive(monitor_channel *ch, void *buf, Size buf_size, Size *out_len)
{
	ShmMqChannelData *priv = (ShmMqChannelData *)ch->private_data;


	Size len;
	bool result = shm_mq_receive(priv->mq_handle, &len, buf, buf_size);
	if (result && out_len)
		*out_len = len;

	return result;
}

static void
shm_mq_channel_cleanup(monitor_channel *ch)
{
	ShmMqChannelData *priv = (ShmMqChannelData *)ch->private_data;

	if (priv->mq_handle)
		shm_mq_detach(priv->mq_handle);

	pfree(priv);
	ch->private_data = NULL;
}
