/*-------------------------------------------------------------------------
 *
 * pgtz.h
 *	  Timezone Library Integration Functions
 *
 * Note: this file contains only definitions that are private to the
 * timezone library.  Public definitions are in pgtime.h.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/pgtz.h,v 1.8 2004/05/21 12:30:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGTZ_H
#define _PGTZ_H

#define TZ_STRLEN_MAX 255

extern char *pg_TZDIR(void);

#endif   /* _PGTZ_H */
