/*-------------------------------------------------------------------------
 *
 * ps_status.h
 *
 *	Defines macros to show backend status on the ps status line.
 *	Unfortunately this is system dpendent.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PS_STATUS_H
#define PS_STATUS_H

#ifdef linux

#include <string.h>

extern char *ps_status_buffer;

#define PS_DEFINE_BUFFER \
char *ps_status_buffer = NULL

#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname) \
	{ \
		int i; \
		for (i = 0; i < (argc); i++) { \
			memset((argv)[i], 0, strlen((argv)[i])); \
		} \
		ps_status_buffer = (argv)[0]; \
		sprintf(ps_status_buffer, "%s %s %s %s ", execname, username, hostname, dbname); \
		ps_status_buffer += strlen(ps_status_buffer); \
		ps_status_buffer[0] = '\0'; \
	}

#define PS_CLEAR_STATUS() \
	{ if (ps_status_buffer) memset(ps_status_buffer, 0, strlen(ps_status_buffer)); }

#define PS_SET_STATUS(status) \
	{ \
		if (ps_status_buffer) \
		{ \
			PS_CLEAR_STATUS(); \
			strcpy(ps_status_buffer, status); \
		} \
	}

#define PS_STATUS (ps_status_buffer ? ps_status_buffer : "")

#else							/* !linux */

extern const char **ps_status;

#define PS_DEFINE_BUFFER \
const char **ps_status = NULL

#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname) \
	{ \
		int i; \
		Assert(argc >= 5); \
		argv[0] = execname; \
		argv[1] = hostname; \
		argv[2] = username; \
		argv[3] = dbname; \
		ps_status = (const char **)&argv[4]; \
		for (i = 4; i < argc; i++) \
			argv[i] = "";  /* blank them */ \
	}

#define PS_CLEAR_STATUS() \
	{ if (ps_status) *ps_status = ""; }

#define PS_SET_STATUS(status) \
	{ if (ps_status) *ps_status = (status); }

#define PS_STATUS (ps_status ? *ps_status : "")
#endif

#ifdef NO_PS_STATUS
#define PS_DEFINE_BUFFER
#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname)
#define PS_CLEAR_STATUS()
#define PS_SET_STATUS(status) { if ((status)); }
#define PS_STATUS ""
#endif	 /* !linux */

#endif	 /* PS_STATUS_H */
