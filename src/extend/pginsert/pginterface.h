/*
 * pglib.h
 *
*/

PGresult *doquery(char *query);
PGconn 	*connectdb();
void	disconnectdb();
int		fetch(void *param, ...);
int		skip_query_errors;

#define END_OF_TUPLES	(-1)
