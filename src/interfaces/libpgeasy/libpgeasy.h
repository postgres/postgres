/*
 * pglib.h
 *
*/

PGresult   *doquery(char *query);
PGconn	   *connectdb(char *options);
void		disconnectdb(void);
int			fetch(void *param,...);
int			fetchwithnulls(void *param,...);
void		on_error_continue(void);
void		on_error_stop(void);
PGresult   *get_result(void);
void		set_result(PGresult *newres);
void		unset_result(PGresult *oldres);
void		reset_fetch(void);

#define END_OF_TUPLES	(-1)
