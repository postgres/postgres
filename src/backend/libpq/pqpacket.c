/*-------------------------------------------------------------------------
 *
 * pqpacket.c--
 *	  routines for reading and writing data packets sent/received by
 *	  POSTGRES clients and servers
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/Attic/pqpacket.c,v 1.10 1997/11/10 05:16:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* NOTES
 *	  This is the module that understands the lowest-level part
 *	  of the communication protocol.  All of the trickiness in
 *	  this module is for making sure that non-blocking I/O in
 *	  the Postmaster works correctly.	Check the notes in PacketRecv
 *	  on non-blocking I/O.
 *
 * Data Structures:
 *		Port has two important functions. (1) It records the
 *		sock/addr used in communication. (2) It holds partially
 *		read in messages.  This is especially important when
 *		we haven't seen enough to construct a complete packet
 *		header.
 *
 * PacketBuf -- None of the clients of this module should know
 *		what goes into a packet hdr (although they know how big
 *		it is).  This routine is in charge of host to net order
 *		conversion for headers.  Data conversion is someone elses
 *		responsibility.
 *
 * IMPORTANT: these routines are called by backends, clients, and
 *		the Postmaster.
 *
 */
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>

#include <postgres.h>
#include <miscadmin.h>
#include <storage/ipc.h>
#include <libpq/libpq.h>

/*
 * PacketReceive -- receive a packet on a port.
 *
 * RETURNS: connection id of the packet sender, if one
 * is available.
 *
 */
int
PacketReceive(Port *port,		/* receive port */
			  PacketBuf *buf,	/* MAX_PACKET_SIZE-worth of buffer space */
			  bool nonBlocking) /* NON_BLOCKING or BLOCKING i/o */
{
	PacketLen	max_size = sizeof(PacketBuf);
	PacketLen	cc;				/* character count -- bytes recvd */
	PacketLen	packetLen;		/* remaining packet chars to read */
	Addr		tmp;			/* curr recv buf pointer */
	int			addrLen = sizeof(struct sockaddr_in);
	int			hdrLen;
	int			flag;
	int			decr;

	hdrLen = sizeof(buf->len);

	if (nonBlocking == NON_BLOCKING)
	{
		flag = MSG_PEEK;
		decr = 0;
	}
	else
	{
		flag = 0;
		decr = hdrLen;
	}

	/*
	 * Assume port->nBytes is zero unless we were interrupted during
	 * non-blocking I/O.  This first recv() is to get the hdr information
	 * so we know how many bytes to read.  Life would be very complicated
	 * if we read too much data (buffering).
	 */
	tmp = ((Addr) buf) + port->nBytes;

	if (port->nBytes >= hdrLen)
	{
		packetLen = ntohl(buf->len) - port->nBytes;
	}
	else
	{
		/* peeking into the incoming message */
		cc = recv(port->sock, (char *) &(buf->len), hdrLen, flag);
		if (cc < hdrLen)
		{
			/* if cc is negative, the system call failed */
			if (cc < 0)
			{
				return (STATUS_ERROR);
			}

			/*
			 * cc == 0 means the connection was broken at the other end.
			 */
			else if (!cc)
			{
				return (STATUS_INVALID);

			}
			else
			{

				/*
				 * Worst case.	We didn't even read in enough data to get
				 * the header length. since we are using a data stream,
				 * this happens only if the client is mallicious.
				 *
				 * Don't save the number of bytes we've read so far. Since we
				 * only peeked at the incoming message, the kernel is
				 * going to keep it for us.
				 */
				return (STATUS_NOT_DONE);
			}
		}
		else
		{

			/*
			 * This is an attempt to shield the Postmaster from mallicious
			 * attacks by placing tighter restrictions on the reported
			 * packet length.
			 *
			 * Check for negative packet length
			 */
			if ((buf->len) <= 0)
			{
				return (STATUS_INVALID);
			}

			/*
			 * Check for oversize packet
			 */
			if ((ntohl(buf->len)) > max_size)
			{
				return (STATUS_INVALID);
			}

			/*
			 * great. got the header. now get the true length (including
			 * header size).
			 */
			packetLen = ntohl(buf->len);

			/*
			 * if someone is sending us junk, close the connection
			 */
			if (packetLen > max_size)
			{
				port->nBytes = packetLen;
				return (STATUS_BAD_PACKET);
			}
			packetLen -= decr;
			tmp += decr - port->nBytes;
		}
	}

	/*
	 * Now that we know how big it is, read the packet.  We read the
	 * entire packet, since the last call was just a peek.
	 */
	while (packetLen)
	{
		cc = read(port->sock, tmp, packetLen);
		if (cc < 0)
			return (STATUS_ERROR);

		/*
		 * cc == 0 means the connection was broken at the other end.
		 */
		else if (!cc)
			return (STATUS_INVALID);

/*
   fprintf(stderr,"expected packet of %d bytes, got %d bytes\n",
		   packetLen, cc);
*/
		tmp += cc;
		packetLen -= cc;

		/* if non-blocking, we're done. */
		if (nonBlocking && packetLen)
		{
			port->nBytes += cc;
			return (STATUS_NOT_DONE);
		}
	}

	port->nBytes = 0;
	return (STATUS_OK);
}

/*
 * PacketSend -- send a single-packet message.
 *
 * RETURNS: STATUS_ERROR if the write fails, STATUS_OK otherwise.
 * SIDE_EFFECTS: may block.
 * NOTES: Non-blocking writes would significantly complicate
 *		buffer management.	For now, we're not going to do it.
 *
 */
int
PacketSend(Port *port,
		   PacketBuf *buf,
		   PacketLen len,
		   bool nonBlocking)
{
	PacketLen	doneLen;

	Assert(!nonBlocking);
	Assert(buf);

	doneLen = write(port->sock, buf, len);
	if (doneLen < len)
	{
		sprintf(PQerrormsg,
		  "FATAL: PacketSend: couldn't send complete packet: errno=%d\n",
				errno);
		fputs(PQerrormsg, stderr);
		return (STATUS_ERROR);
	}

	return (STATUS_OK);
}

/*
 * StartupInfo2PacketBuf -
 *	 convert the fields of the StartupInfo to a PacketBuf
 *
 */
/* moved to src/libpq/fe-connect.c */
/*
PacketBuf*
StartupInfo2PacketBuf(StartupInfo* s)
{
  PacketBuf* res;
  char* tmp;

  res = (PacketBuf*)malloc(sizeof(PacketBuf));
  res->len = htonl(sizeof(PacketBuf));
  res->data[0] = '\0';

  tmp= res->data;

  strncpy(tmp, s->database, sizeof(s->database));
  tmp += sizeof(s->database);
  strncpy(tmp, s->user, sizeof(s->user));
  tmp += sizeof(s->user);
  strncpy(tmp, s->options, sizeof(s->options));
  tmp += sizeof(s->options);
  strncpy(tmp, s->execFile, sizeof(s->execFile));
  tmp += sizeof(s->execFile);
  strncpy(tmp, s->tty, sizeof(s->execFile));

  return res;
}
*/

/*
 * PacketBuf2StartupInfo -
 *	 convert the fields of the StartupInfo to a PacketBuf
 *
 */
/* moved to postmaster.c
StartupInfo*
PacketBuf2StartupInfo(PacketBuf* p)
{
  StartupInfo* res;
  char* tmp;

  res = (StartupInfo*)malloc(sizeof(StartupInfo));

  res->database[0]='\0';
  res->user[0]='\0';
  res->options[0]='\0';
  res->execFile[0]='\0';
  res->tty[0]='\0';

  tmp= p->data;
  strncpy(res->database,tmp,sizeof(res->database));
  tmp += sizeof(res->database);
  strncpy(res->user,tmp, sizeof(res->user));
  tmp += sizeof(res->user);
  strncpy(res->options,tmp, sizeof(res->options));
  tmp += sizeof(res->options);
  strncpy(res->execFile,tmp, sizeof(res->execFile));
  tmp += sizeof(res->execFile);
  strncpy(res->tty,tmp, sizeof(res->tty));

  return res;
}
*/
