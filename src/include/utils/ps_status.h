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

#ifdef HAVE_SETPROCTITLE

extern char Ps_status_buffer[];

#undef PS_DEFINE_BUFFER

#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname) \
        do { \
                sprintf(Ps_status_buffer, "%s %s %s %s", execname, hostname, username, dbname); \
        } while (0)

#define PS_CLEAR_STATUS() \
        do { setproctitle("%s", Ps_status_buffer); } while (0)

#define PS_SET_STATUS(status) \
        do { setproctitle("%s %s", Ps_status_buffer, (status)); } while (0)

#define PS_STATUS (Ps_status_buffer)

#elif defined(linux)

#include <string.h>

extern char *ps_status_buffer;

#define PS_DEFINE_BUFFER \
char *ps_status_buffer = NULL

#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname) \
	do { \
		int i; \
		for (i = 0; i < (argc); i++) { \
			memset((argv)[i], 0, strlen((argv)[i])); \
		} \
		ps_status_buffer = (argv)[0]; \
		sprintf(ps_status_buffer, "%s %s %s %s ", execname, username, hostname, dbname); \
		ps_status_buffer += strlen(ps_status_buffer); \
		ps_status_buffer[0] = '\0'; \
	} while (0)

#define PS_CLEAR_STATUS() \
	do { \
		if (ps_status_buffer) \
			memset(ps_status_buffer, 0, strlen(ps_status_buffer)); \
	} while (0)

#define PS_SET_STATUS(status) \
	do { \
		if (ps_status_buffer) \
		{ \
			PS_CLEAR_STATUS(); \
			strcpy(ps_status_buffer, status); \
		} \
	} while (0)

#define PS_STATUS (ps_status_buffer ? ps_status_buffer : "")

#else							/* !linux */

extern char Ps_status_buffer[];

#undef PS_DEFINE_BUFFER

#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname) \
	do { \
		int i; \
		Assert(argc >= 5); \
		argv[0] = execname; \
		argv[1] = hostname; \
		argv[2] = username; \
		argv[3] = dbname; \
		argv[4] = Ps_status_buffer; \
		for (i = 5; i < argc; i++) \
			argv[i] = "";  /* blank them */ \
	} while (0)

#define PS_CLEAR_STATUS() \
	do { Ps_status_buffer[0] = '\0'; } while (0)

#define PS_SET_STATUS(status) \
	do { strcpy(Ps_status_buffer, (status)); } while (0)

#define PS_STATUS (Ps_status_buffer)
#endif

#ifdef NO_PS_STATUS
#undef PS_DEFINE_BUFFER
#define PS_INIT_STATUS(argc, argv, execname, username, hostname, dbname)
#define PS_CLEAR_STATUS()
#define PS_SET_STATUS(status) do { if ((status)); } while (0)
#define PS_STATUS ""
#endif	 /* !linux */

#endif	 /* PS_STATUS_H */
