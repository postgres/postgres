/* -------------------------------------------------------------------------
 *
 * monitor_event.h
 *	  Routines for interprocess monitoring events
 *
 * src/include/monitorsubsystem/monitor_event.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * тут будет лежать интерфейс / API
 * (объявление)
 * в .с - реализация
 */

#ifndef MONITOR_EVENT_H
#define MONITOR_EVENT_H
#include "postmaster/monitor.h"

typedef enum
{
    MSS_OK,

    MSS_ERR_NOT_REGISTERED,
    MSS_ERR_INVALID_ARG,
    MSS_ERR_NO_SUBJECTS_SLOTS_AVAILABLE ,
    MSS_ERR_ROUTING_MISMATCH ,
    MSS_ERR_ALREADY_SUBSCRIBED,
} MSS_SUBSCRIBE_RESULT;



// con = Consumer / Subscriber
int pg_monitor_con_connect(MonitorChannelConfig *conConfig);
void pg_monitor_con_disconnect();

MSS_SUBSCRIBE_RESULT pg_monitor_subscribe_to_event(const char *event_string, routing_type _routing_type);
void pg_monitor_unsubscribe_from_event(const char *event_string);

// pub = Publisher
int pg_monitor_pub_connect(MonitorChannelConfig *conConfig);
void pg_monitor_pub_disconnect();

// в случае, если не удалось уведомить о событии, быстро возврщает управление
int pg_monitor_fast_notify(const char *event_string);
// пытается уведомить о событии вне зависимости от времени ожидания
// в случае смерти процесса ПСМ возвращает управление
int pg_monitor_reliable_notify(const char *event_string);

#endif /* MONITOR_EVENT_H */
