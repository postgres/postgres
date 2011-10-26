#include "access/xlogdefs.h"

/*
 * Called whenever a segment is finished, return true to stop
 * the streaming at this point.
 */
typedef bool (*segment_finish_callback)(XLogRecPtr segendpos, uint32 timeline);

/*
 * Called before trying to read more data. Return true to stop
 * the streaming at this point.
 */
typedef bool (*stream_continue_callback)(void);

extern bool ReceiveXlogStream(PGconn *conn,
							  XLogRecPtr startpos,
							  uint32 timeline,
							  char *sysidentifier,
							  char *basedir,
							  segment_finish_callback segment_finish,
							  stream_continue_callback stream_continue,
							  int standby_message_timeout);
