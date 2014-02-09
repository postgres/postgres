#include "libpq-fe.h"

extern const char *progname;
extern char *dbhost;
extern char *dbuser;
extern char *dbport;
extern int	dbgetpassword;

/* Connection kept global so we can disconnect easily */
extern PGconn *conn;

char	   *xstrdup(const char *s);
void	   *xmalloc0(int size);

PGconn	   *GetConnection(void);
