
#ifndef TCPIPV4
#define TCPIPV4
#endif	 /* */

#ifndef MAXSOCKETS
#define MAXSOCKETS 2048
#endif	 /* */

/*
 * DEF_PGPORT is the TCP port number on which the Postmaster listens by
 * default.  This can be overriden by command options, environment variables,
 * and the postconfig hook. (set by build script)
	  */

#define DEF_PGPORT "5432"

#define HAVE_TERMIOS_H
#define HAVE_ENDIAN_H

#define SOCKET_SIZE_TYPE size_t

#define  strcasecmp(s1, s2) stricmp(s1, s2)



