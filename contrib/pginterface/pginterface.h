/*
 * pglib.h
 *
*/

PGresult *doquery(char *query);
PGconn 	*connectdb();
void	disconnectdb();
int	fetch(void *param, ...);
int	fetchisnull(void *param, ...);
void	on_error_continue();
void	on_error_stop();

#define END_OF_TUPLES	(-1)
