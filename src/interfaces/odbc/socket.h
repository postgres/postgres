
/* File:			socket.h
 *
 * Description:		See "socket.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __SOCKET_H__
#define __SOCKET_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define closesocket(xxx) close(xxx)
#define SOCKETFD int

#ifndef		  INADDR_NONE
#ifndef _IN_ADDR_T
#define _IN_ADDR_T
typedef unsigned int in_addr_t;

#endif
#define INADDR_NONE ((in_addr_t)-1)
#endif

#else
#include <winsock.h>
#define SOCKETFD SOCKET
#endif

#include "psqlodbc.h"

#define SOCKET_ALREADY_CONNECTED 1
#define SOCKET_HOST_NOT_FOUND 2
#define SOCKET_COULD_NOT_CREATE_SOCKET 3
#define SOCKET_COULD_NOT_CONNECT 4
#define SOCKET_READ_ERROR 5
#define SOCKET_WRITE_ERROR 6
#define SOCKET_NULLPOINTER_PARAMETER 7
#define SOCKET_PUT_INT_WRONG_LENGTH 8
#define SOCKET_GET_INT_WRONG_LENGTH 9
#define SOCKET_CLOSED 10


struct SocketClass_
{

	int			buffer_filled_in;
	int			buffer_filled_out;
	int			buffer_read_in;
	unsigned char *buffer_in;
	unsigned char *buffer_out;

	SOCKETFD	socket;

	char	   *errormsg;
	int			errornumber;

	char		reverse;		/* used to handle Postgres 6.2 protocol
								 * (reverse byte order) */

};

#define SOCK_get_char(self)		(SOCK_get_next_byte(self))
#define SOCK_put_char(self, c)	(SOCK_put_next_byte(self, c))


/* error functions */
#define SOCK_get_errcode(self)		(self->errornumber)
#define SOCK_get_errmsg(self)		(self->errormsg)


/* Socket prototypes */
SocketClass *SOCK_Constructor(void);
void		SOCK_Destructor(SocketClass *self);
char		SOCK_connect_to(SocketClass *self, unsigned short port, char *hostname);
void		SOCK_get_n_char(SocketClass *self, char *buffer, int len);
void		SOCK_put_n_char(SocketClass *self, char *buffer, int len);
void		SOCK_get_string(SocketClass *self, char *buffer, int bufsize);
void		SOCK_put_string(SocketClass *self, char *string);
int			SOCK_get_int(SocketClass *self, short len);
void		SOCK_put_int(SocketClass *self, int value, short len);
void		SOCK_flush_output(SocketClass *self);
unsigned char SOCK_get_next_byte(SocketClass *self);
void		SOCK_put_next_byte(SocketClass *self, unsigned char next_byte);
void		SOCK_clear_error(SocketClass *self);

#endif
