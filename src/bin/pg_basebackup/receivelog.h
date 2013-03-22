#include "access/xlogdefs.h"

/*
 * Called before trying to read more data or when a segment is
 * finished. Return true to stop streaming.
 */
typedef bool (*stream_stop_callback) (XLogRecPtr segendpos, uint32 timeline, bool segment_finished);

extern bool CheckServerVersionForStreaming(PGconn *conn);
extern bool ReceiveXlogStream(PGconn *conn,
				  XLogRecPtr startpos,
				  uint32 timeline,
				  char *sysidentifier,
				  char *basedir,
				  stream_stop_callback stream_stop,
				  int standby_message_timeout,
				  char *partial_suffix);
