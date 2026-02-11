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
#include "postmaster/monitor.h"
#include "monitorsubsystem/monitor_channel.h"
#include "monitorsubsystem/monitor_channel_type.h"
#include "monitorsubsystem/monitor_channel_shm_mq.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"


/* static needed ? */
static bool
/*
 * Есть shm_mq_handle - это backend-local структура для уже 
 * существующей shm_mq, через который конкретный процесс
 * будет с ней работать
 * 
 * Контекст памяти, активный в момент создания shm_mq_attach,
 * должен прожить как минимум столько же, сколько и сама shm_mq
 * 
 * есть штучка: для shm_mq нет функций типа reconnect / reset_queue / replace_sender
 * 
 * там могут (не факт, что возникнут, но могут) возникнуть проблемы с ожиданием
 * Latch'ей и состояниями очереди (в shm_mq кольцевой буфер и тд - если что-то случится)
 * с записью посреди записи, то все может быть нехорошо...
 * (надо рассмотреть эту ситуацию, я ее пока не рассматривала)
 * то есть внутреннее состояние shm_mq может быть не reset-safe
 * 
 *  
 * src/test/modules/test_shm_mq/setup.c
 * строка 147 - пример как создавать shm_mq с помощью
 * toc и shm_toc_insert в DSM
 * 
 * TODO:
 * add about here or in shm_mq_channel_attach
 * initializing shm_mq_set_receiver and shm_mq_set_sender
 */
shm_mq_channel_init(monitor_channel *ch, MonitorChannelConfig *cfg)
{
	shm_toc *toc = monSubSysLocal.MonSubSystem_SharedState->channels_toc;
	Size sz = cfg->u.shm_mq.mq_size + sizeof(ShmMqChannelData) + sizeof(ShmMqChannelLocal);
	ShmMqChannelData *data;
	void *mq_space;

	data = shm_toc_allocate(toc, sz);
	mq_space = (void *)data + sizeof(ShmMqChannelData) + sizeof(ShmMqChannelLocal);

	data->mq = shm_mq_create(mq_space, cfg->u.shm_mq.mq_size);

	ch->private_data = data;
	ch->ops = &ShmMqChannelOps;

	shm_toc_insert(toc, cfg->channel_id, data);
	return true;
}

/*
 * TODO:
 * think about MemoryContext for operations with channels
 */
void
shm_mq_channel_attach(monitor_channel *ch, ChannelRole role)
{
    ShmMqChannelData *data = ch->private_data;
    ShmMqChannelLocal *local;

	Assert(role == Publisher || role == Subscriber);

	/* Here shold be smth with MemoryContext */
    local = palloc0(sizeof(ShmMqChannelLocal));
    local->handle = shm_mq_attach(data->mq, NULL, NULL);

    if (role == Subscriber) {
        monSubSysLocal.subLocalData = local;
	}
    else {
        monSubSysLocal.pubLocalData = local;
	}
	return;
}

/*
 * TODO:
 * mind all checks
 */
static bool
shm_mq_channel_send_msg(monitor_channel *ch, const void *data, Size len)
{
    ShmMqChannelLocal *local;
	shm_mq_result result;

    local = (ShmMqChannelLocal *)
        monSubSysLocal.pubLocalData;

    Assert(local && local->handle);

    

    result = shm_mq_send(local->handle,
                         len,
                         data,
                         false,  /* nowait = false (block) */
                         false); /* force_flush */

	switch (result)
	{
	case SHM_MQ_SUCCESS:
		return true;
		break;
	case SHM_MQ_DETACHED:
	case SHM_MQ_WOULD_BLOCK:
		return false;
		break;
	
	default:
		elog(ERROR, "unexpected shm_mq_send result");
    	return false;
		break;
	}
}


/*
 * TODO:
 * mind all checks
 */
static ChannelRecvResult
shm_mq_channel_receive_msg(monitor_channel *ch, void *buf, Size buf_size, Size *out_len)
{
    ShmMqChannelLocal *local;
    shm_mq_result result;
    Size len;
    void *data;

    /* Берём локальный handle */
    local = (ShmMqChannelLocal *)
        monSubSysLocal.subLocalData;

    Assert(local && local->handle);

    result = shm_mq_receive(local->handle,
                            &len,
                            &data,
                            true);   /* nowait */

    if (result == SHM_MQ_WOULD_BLOCK)
        return CHANNEL_RECV_EMPTY;

    if (result == SHM_MQ_DETACHED)
        return CHANNEL_RECV_CLOSED;

    if (result != SHM_MQ_SUCCESS)
        elog(ERROR, "unexpected shm_mq_receive result");

    if (len > buf_size)
        elog(ERROR, "message too large for buffer");

    memcpy(buf, data, len);

    if (out_len)
        *out_len = len;

    return CHANNEL_RECV_OK;
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
