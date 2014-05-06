#include "libpq-fe.h"

extern const char *progname;
extern char *connection_string;
extern char *dbhost;
extern char *dbuser;
extern char *dbport;
extern char *dbname;
extern int	dbgetpassword;
extern char *replication_slot;

/* Connection kept global so we can disconnect easily */
extern PGconn *conn;

extern PGconn *GetConnection(void);

extern int64 feGetCurrentTimestamp(void);
extern void feTimestampDifference(int64 start_time, int64 stop_time,
					  long *secs, int *microsecs);

extern bool feTimestampDifferenceExceeds(int64 start_time, int64 stop_time,
							 int msec);
extern void fe_sendint64(int64 i, char *buf);
extern int64 fe_recvint64(char *buf);
