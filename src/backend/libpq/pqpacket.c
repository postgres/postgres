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
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/Attic/pqpacket.c,v 1.16 1998/07/09 03:28:46 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
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
 * Set up a packet read for the postmaster event loop.
 */

void		PacketReceiveSetup(Packet *pkt, PacketDoneProc iodone, void *arg)
{
	pkt->nrtodo = sizeof(pkt->len);
	pkt->ptr = (char *) &pkt->len;
	pkt->iodone = iodone;
	pkt->arg = arg;
	pkt->state = ReadingPacketLength;

	/* Clear the destination. */

	MemSet(&pkt->pkt, 0, sizeof(pkt->pkt));
}


/*
 * Read a packet fragment.	Return STATUS_OK if the connection should stay
 * open.
 */

int
PacketReceiveFragment(Packet *pkt, int sock)
{
	int			got;

	if ((got = read(sock, pkt->ptr, pkt->nrtodo)) > 0)
	{
		pkt->nrtodo -= got;
		pkt->ptr += got;

		/* See if we have got what we need for the packet length. */

		if (pkt->nrtodo == 0 && pkt->state == ReadingPacketLength)
		{
			pkt->len = ntohl(pkt->len);

			if (pkt->len < sizeof(pkt->len) ||
				pkt->len > sizeof(pkt->len) + sizeof(pkt->pkt))
			{
				PacketSendError(pkt, "Invalid packet length");

				return STATUS_OK;
			}

			/* Set up for the rest of the packet. */

			pkt->nrtodo = pkt->len - sizeof(pkt->len);
			pkt->ptr = (char *) &pkt->pkt;
			pkt->state = ReadingPacket;
		}

		/* See if we have got what we need for the packet. */

		if (pkt->nrtodo == 0 && pkt->state == ReadingPacket)
		{
			pkt->state = Idle;

			/* Special case to close the connection. */

			if (pkt->iodone == NULL)
				return STATUS_ERROR;

			return (*pkt->iodone) (pkt->arg, pkt->len - sizeof(pkt->len),
								   (void *) &pkt->pkt);
		}

		return STATUS_OK;
	}

	if (got == 0)
		return STATUS_ERROR;

	if (errno == EINTR)
		return STATUS_OK;

	perror("PacketReceiveFragment: read() failed");

	return STATUS_ERROR;
}


/*
 * Set up a packet write for the postmaster event loop.
 */

void		PacketSendSetup(Packet *pkt, int nbytes, PacketDoneProc iodone, void *arg)
{
	pkt->len = (PacketLen) nbytes;
	pkt->nrtodo = nbytes;
	pkt->ptr = (char *) &pkt->pkt;
	pkt->iodone = iodone;
	pkt->arg = arg;
	pkt->state = WritingPacket;
}


/*
 * Write a packet fragment.  Return STATUS_OK if the connection should stay
 * open.
 */

int
PacketSendFragment(Packet *pkt, int sock)
{
	int			done;

	if ((done = write(sock, pkt->ptr, pkt->nrtodo)) > 0)
	{
		pkt->nrtodo -= done;
		pkt->ptr += done;

		/* See if we have written the whole packet. */

		if (pkt->nrtodo == 0)
		{
			pkt->state = Idle;

			/* Special case to close the connection. */

			if (pkt->iodone == NULL)
				return STATUS_ERROR;

			return (*pkt->iodone) (pkt->arg, pkt->len,
								   (void *) &pkt->pkt);
		}

		return STATUS_OK;
	}

	if (done == 0)
		return STATUS_ERROR;

	if (errno == EINTR)
		return STATUS_OK;

	perror("PacketSendFragment: write() failed");

	return STATUS_ERROR;
}


/*
 * Send an error message from the postmaster to the frontend.
 */

void
PacketSendError(Packet *pkt, char *errormsg)
{
	fprintf(stderr, "%s\n", errormsg);

	pkt->pkt.em.data[0] = 'E';
	StrNCpy(&pkt->pkt.em.data[1], errormsg, sizeof(pkt->pkt.em.data) - 2);

	/*
	 * The NULL i/o callback will cause the connection to be broken when
	 * the error message has been sent.
	 */

	PacketSendSetup(pkt, strlen(pkt->pkt.em.data) + 1, NULL, NULL);
}
