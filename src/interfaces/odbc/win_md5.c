/*
 *		win_md5.c
 *	Under Windows I don't love the following /D in makefiles. - inoue
 */
#define MD5_ODBC
#define FRONTEND

/*	
 *	md5.c is the exact copy of the src/backend/libpq/md5.c.
 *
 *		psqlodbc driver stuff never refer(link) to other
 *		stuff directly.		
 *	
 */
#include "md5.c"
