#include "libpq-fe.h"

extern const char *progname;
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


extern char *pg_strdup(const char *s);
extern void *pg_malloc0(size_t size);

extern PGconn *GetConnection(void);
