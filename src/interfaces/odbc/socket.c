/* Module:			socket.c
 *
 * Description:		This module contains functions for low level socket
 *					operations (connecting/reading/writing to the backend)
 *
 * Classes:			SocketClass (Functions prefix: "SOCK_")
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "socket.h"

#ifndef WIN32
#include <stdlib.h>
#include <string.h>				/* for memset */
#endif

extern GLOBAL_VALUES globals;

#ifndef BOOL
#define BOOL	int
#endif
#ifndef TRUE
#define TRUE	(BOOL)1
#endif
#ifndef FALSE
#define FALSE	(BOOL)0
#endif


void
SOCK_clear_error(SocketClass * self)
{
	self->errornumber = 0;
	self->errormsg = NULL;
}

SocketClass *
SOCK_Constructor()
{
	SocketClass *rv;

	rv = (SocketClass *) malloc(sizeof(SocketClass));

	if (rv != NULL)
	{
		rv->socket = (SOCKETFD) - 1;
		rv->buffer_filled_in = 0;
		rv->buffer_filled_out = 0;
		rv->buffer_read_in = 0;

		rv->buffer_in = (unsigned char *) malloc(globals.socket_buffersize);
		if (!rv->buffer_in)
			return NULL;

		rv->buffer_out = (unsigned char *) malloc(globals.socket_buffersize);
		if (!rv->buffer_out)
			return NULL;

		rv->errormsg = NULL;
		rv->errornumber = 0;

		rv->reverse = FALSE;
	}
	return rv;
}

void
SOCK_Destructor(SocketClass * self)
{
	if (self->socket != -1)
	{
		if (!shutdown(self->socket, 2)) /* no sends or receives */
		{
			SOCK_put_char(self, 'X');
			SOCK_flush_output(self);
			closesocket(self->socket);
		}
	}

	if (self->buffer_in)
		free(self->buffer_in);

	if (self->buffer_out)
		free(self->buffer_out);

	free(self);
}


char
SOCK_connect_to(SocketClass * self, unsigned short port, char *hostname)
{
	struct hostent *host;
	struct sockaddr_in sadr;
	unsigned long iaddr;

	if (self->socket != -1)
	{
		self->errornumber = SOCKET_ALREADY_CONNECTED;
		self->errormsg = "Socket is already connected";
		return 0;
	}

	memset((char *) &sadr, 0, sizeof(sadr));

	/*
	 * If it is a valid IP address, use it. Otherwise use hostname lookup.
	 */
	iaddr = inet_addr(hostname);
	if (iaddr == INADDR_NONE)
	{
		host = gethostbyname(hostname);
		if (host == NULL)
		{
			self->errornumber = SOCKET_HOST_NOT_FOUND;
			self->errormsg = "Could not resolve hostname.";
			return 0;
		}
		memcpy(&(sadr.sin_addr), host->h_addr, host->h_length);
	}
	else
		memcpy(&(sadr.sin_addr), (struct in_addr *) & iaddr, sizeof(iaddr));

	sadr.sin_family = AF_INET;
	sadr.sin_port = htons(port);

	self->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (self->socket == -1)
	{
		self->errornumber = SOCKET_COULD_NOT_CREATE_SOCKET;
		self->errormsg = "Could not create Socket.";
		return 0;
	}

	if (connect(self->socket, (struct sockaddr *) & (sadr),
				sizeof(sadr)) < 0)
	{
		self->errornumber = SOCKET_COULD_NOT_CONNECT;
		self->errormsg = "Could not connect to remote socket.";
		closesocket(self->socket);
		self->socket = (SOCKETFD) - 1;
		return 0;
	}
	return 1;
}


void
SOCK_get_n_char(SocketClass * self, char *buffer, int len)
{
	int			lf;

	if (!buffer)
	{
		self->errornumber = SOCKET_NULLPOINTER_PARAMETER;
		self->errormsg = "get_n_char was called with NULL-Pointer";
		return;
	}

	for (lf = 0; lf < len; lf++)
		buffer[lf] = SOCK_get_next_byte(self);
}


void
SOCK_put_n_char(SocketClass * self, char *buffer, int len)
{
	int			lf;

	if (!buffer)
	{
		self->errornumber = SOCKET_NULLPOINTER_PARAMETER;
		self->errormsg = "put_n_char was called with NULL-Pointer";
		return;
	}

	for (lf = 0; lf < len; lf++)
		SOCK_put_next_byte(self, (unsigned char) buffer[lf]);
}


/*	bufsize must include room for the null terminator
	will read at most bufsize-1 characters + null.
*/
void
SOCK_get_string(SocketClass * self, char *buffer, int bufsize)
{
	register int lf = 0;

	for (lf = 0; lf < bufsize; lf++)
		if (!(buffer[lf] = SOCK_get_next_byte(self)))
			return;

	buffer[bufsize - 1] = '\0';
}


void
SOCK_put_string(SocketClass * self, char *string)
{
	register int lf;
	int			len;

	len = strlen(string) + 1;

	for (lf = 0; lf < len; lf++)
		SOCK_put_next_byte(self, (unsigned char) string[lf]);
}


int
SOCK_get_int(SocketClass * self, short len)
{
	char		buf[4];

	switch (len)
	{
		case 2:
			SOCK_get_n_char(self, buf, len);
			if (self->reverse)
				return *((unsigned short *) buf);
			else
				return ntohs(*((unsigned short *) buf));

		case 4:
			SOCK_get_n_char(self, buf, len);
			if (self->reverse)
				return *((unsigned int *) buf);
			else
				return ntohl(*((unsigned int *) buf));

		default:
			self->errornumber = SOCKET_GET_INT_WRONG_LENGTH;
			self->errormsg = "Cannot read ints of that length";
			return 0;
	}
}


void
SOCK_put_int(SocketClass * self, int value, short len)
{
	unsigned int rv;

	switch (len)
	{
		case 2:
			rv = self->reverse ? value : htons((unsigned short) value);
			SOCK_put_n_char(self, (char *) &rv, 2);
			return;

		case 4:
			rv = self->reverse ? value : htonl((unsigned int) value);
			SOCK_put_n_char(self, (char *) &rv, 4);
			return;

		default:
			self->errornumber = SOCKET_PUT_INT_WRONG_LENGTH;
			self->errormsg = "Cannot write ints of that length";
			return;
	}
}


void
SOCK_flush_output(SocketClass * self)
{
	int			written;

	written = send(self->socket, (char *) self->buffer_out, self->buffer_filled_out, 0);
	if (written != self->buffer_filled_out)
	{
		self->errornumber = SOCKET_WRITE_ERROR;
		self->errormsg = "Could not flush socket buffer.";
	}
	self->buffer_filled_out = 0;
}

unsigned char
SOCK_get_next_byte(SocketClass * self)
{
	if (self->buffer_read_in >= self->buffer_filled_in)
	{
		/* there are no more bytes left in the buffer -> */
		/* reload the buffer */

		self->buffer_read_in = 0;
		self->buffer_filled_in = recv(self->socket, (char *) self->buffer_in, globals.socket_buffersize, 0);

		mylog("read %d, global_socket_buffersize=%d\n", self->buffer_filled_in, globals.socket_buffersize);

		if (self->buffer_filled_in == -1)
		{
			self->errornumber = SOCKET_READ_ERROR;
			self->errormsg = "Error while reading from the socket.";
			self->buffer_filled_in = 0;
		}
		if (self->buffer_filled_in == 0)
		{
			self->errornumber = SOCKET_CLOSED;
			self->errormsg = "Socket has been closed.";
			self->buffer_filled_in = 0;
		}
	}
	return self->buffer_in[self->buffer_read_in++];
}

void
SOCK_put_next_byte(SocketClass * self, unsigned char next_byte)
{
	int			bytes_sent;

	self->buffer_out[self->buffer_filled_out++] = next_byte;

	if (self->buffer_filled_out == globals.socket_buffersize)
	{
		/* buffer is full, so write it out */
		bytes_sent = send(self->socket, (char *) self->buffer_out, globals.socket_buffersize, 0);
		if (bytes_sent != globals.socket_buffersize)
		{
			self->errornumber = SOCKET_WRITE_ERROR;
			self->errormsg = "Error while writing to the socket.";
		}
		self->buffer_filled_out = 0;
	}
}
