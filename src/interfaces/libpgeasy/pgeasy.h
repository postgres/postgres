/*
 * pglib.h
 *
*/

PGresult   *doquery(char *query);
PGconn	   *connectdb();
void		disconnectdb();
int			fetch(void *param,...);
int			fetchwithnulls(void *param,...);
void		on_error_continue();
void		on_error_stop();
PGresult   *get_result();
void		set_result(PGresult *newres);
void		unset_result(PGresult *oldres);
void		reset_fetch();

#define END_OF_TUPLES	(-1)
