/*-------------------------------------------------------------------------
 *
 * ps_status.h
 *
 * Declarations for backend/utils/misc/ps_status.c
 *
 * $PostgreSQL: pgsql/src/include/utils/ps_status.h,v 1.24 2003/11/29 22:41:16 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PS_STATUS_H
#define PS_STATUS_H

extern void save_ps_display_args(int argc, char *argv[]);

extern void init_ps_display(const char *username, const char *dbname,
				const char *host_info);

extern void set_ps_display(const char *activity);

extern const char *get_ps_display(void);

#endif   /* PS_STATUS_H */
