#include "libpq-fe.h"

extern const char *progname;
extern char *connection_string;
extern char *dbhost;
extern char *dbuser;
extern char *dbport;
extern int	dbgetpassword;
extern char *replication_slot;

/* Connection kept global so we can disconnect easily */
extern PGconn *conn;

extern PGconn *GetConnection(void);
