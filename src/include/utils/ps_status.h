/*-------------------------------------------------------------------------
 *
 * ps_status.h
 *
 * Declarations for backend/utils/misc/ps_status.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef PS_STATUS_H
#define PS_STATUS_H

void init_ps_display(int argc, char *argv[],
				const char *username, const char *dbname,
				const char *host_info);

void
			set_ps_display(const char *value);

const char *
			get_ps_display(void);

#endif	 /* PS_STATUS_H */
