/*-------------------------------------------------------------------------
 *
 * ps_status.h
 *
 * Declarations for backend/utils/misc/ps_status.c
 *
 * src/include/utils/ps_status.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PS_STATUS_H
#define PS_STATUS_H

/* disabled on Windows as the performance overhead can be significant */
#ifdef WIN32
#define DEFAULT_UPDATE_PROCESS_TITLE false
#else
#define DEFAULT_UPDATE_PROCESS_TITLE true
#endif

extern PGDLLIMPORT bool update_process_title;

extern char **save_ps_display_args(int argc, char **argv);

extern void init_ps_display(const char *fixed_part);

extern void set_ps_display(const char *activity);

extern const char *get_ps_display(int *displen);

#endif							/* PS_STATUS_H */
