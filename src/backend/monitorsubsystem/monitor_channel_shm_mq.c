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
 * то есть варианты:
 * 1 создать shm_mq'шки в обычной разделяемой памяти
 * + в теории можно просто доподключиться к очереди при рестарте процесса
 * - мб невозможно доподключиться к очереди при рестарте процесса, тк
 *   внутр состояние shm_mq может быть не reset-safe
 * 
 * возможно, можно после рестарта процесса (любого) пытаться переподключиться
 * к очереди, а если с ней проблемы - то ее пересоздать
 * 
 * 2 создать shm_mq'шки в dsm
 * 
 *  
 * src/test/modules/test_shm_mq/setup.c
 * строка 147 - пример как создавать shm_mq с помощью
 * toc и shm_toc_insert в DSM
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
