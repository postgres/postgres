#include "libpq-fe.h"

extern const char *progname;
extern char *connection_string;
extern char *dbhost;
extern char *dbuser;
extern char *dbport;
extern int	dbgetpassword;

/* Connection kept global so we can disconnect easily */
extern PGconn *conn;

#define disconnect_and_exit(code)				\
	{											\
	if (conn != NULL) PQfinish(conn);			\
	exit(code);									\
	}

extern PGconn *GetConnection(void);
