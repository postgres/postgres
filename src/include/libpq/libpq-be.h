/*-------------------------------------------------------------------------
 *
 * libpq-be.h--
 *	  This file contains definitions for structures and
 *	  externs for functions used by the POSTGRES backend.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-be.h,v 1.11 1998/07/09 03:29:00 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

#include <stdio.h>
#include <sys/types.h>

#include "libpq/pqcomm.h"
#include "libpq/hba.h"


/* Protocol v0 password packet. */

typedef struct PasswordPacketV0
{
	uint32		unused;
	char		data[288];		/* User and password as strings. */
} PasswordPacketV0;


/*
 * Password packet.  The length of the password can be changed without
 * affecting anything.
 */

typedef struct PasswordPacket
{
	char		passwd[100];	/* The password. */
} PasswordPacket;


/* Error message packet. */

typedef struct ErrorMessagePacket
{
	char		data[1 + 100];	/* 'E' + the message. */
} ErrorMessagePacket;


/* Authentication request packet. */

typedef struct AuthRequestPacket
{
	char		data[1 + sizeof(AuthRequest) + 2];		/* 'R' + the request +
														 * optional salt. */
} AuthRequestPacket;


/* These are used by the packet handling routines. */

typedef enum
{
	Idle,
	ReadingPacketLength,
	ReadingPacket,
	WritingPacket
} PacketState;

typedef int (*PacketDoneProc) (void * arg, PacketLen pktlen, void * pktdata);

typedef struct Packet
{
	PacketState state;			/* What's in progress. */
	PacketLen	len;			/* Actual length */
	int			nrtodo;			/* Bytes still to transfer */
	char	   *ptr;			/* Buffer pointer */
	PacketDoneProc	iodone;		/* I/O complete callback */
	void	   *arg;			/* Argument to callback */

	/* We declare the data buffer as a union of the allowed packet types,
	 * mainly to ensure that enough space is allocated for the largest one.
	 */

	union
	{
		/* These are outgoing so have no packet length prepended. */

		ErrorMessagePacket em;
		AuthRequestPacket ar;

		/* These are incoming and have a packet length prepended. */

		StartupPacket si;
		CancelRequestPacket canc;
		PasswordPacketV0 pwv0;
		PasswordPacket pw;
	}			pkt;
} Packet;


/*
 * This is used by the postmaster in its communication with frontends.	It is
 * contains all state information needed during this communication before the
 * backend is run.
 */

typedef struct Port
{
	int			sock;			/* File descriptor */
	Packet		pktInfo;		/* For the packet handlers */
	SockAddr	laddr;			/* local addr (us) */
	SockAddr	raddr;			/* remote addr (them) */
	char		salt[2];		/* Password salt */

	/*
	 * Information that needs to be held during the fe/be authentication
	 * handshake.
	 */

	ProtocolVersion proto;
	char		database[SM_DATABASE + 1];
	char		user[SM_USER + 1];
	char		options[SM_OPTIONS + 1];
	char		tty[SM_TTY + 1];
	char		auth_arg[MAX_AUTH_ARG];
	UserAuth	auth_method;
} Port;


extern FILE *Pfout,
		   *Pfin;
extern ProtocolVersion FrontendProtocol;


/*
 * prototypes for functions in pqpacket.c
 */
void		PacketReceiveSetup(Packet *pkt, PacketDoneProc iodone, void *arg);
int			PacketReceiveFragment(Packet *pkt, int sock);
void		PacketSendSetup(Packet *pkt, int nbytes, PacketDoneProc iodone, void *arg);
int			PacketSendFragment(Packet *pkt, int sock);
void		PacketSendError(Packet *pkt, char *errormsg);

#endif							/* LIBPQ_BE_H */
