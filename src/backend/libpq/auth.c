/*-------------------------------------------------------------------------
 *
 * auth.c--
 *	  Routines to handle network authentication
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/auth.c,v 1.32 1998/12/14 05:18:56 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *	   backend (postmaster) routines:
 *		be_recvauth				receive authentication information
 */
#include <stdio.h>
#include <string.h>
#include <sys/param.h>			/* for MAXHOSTNAMELEN on most */
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>				/* for MAXHOSTNAMELEN on some */
#endif
#include <pwd.h>
#include <ctype.h>				/* isspace() declaration */

#include <sys/types.h>			/* needed by in.h on Ultrix */
#include <netinet/in.h>
#include <arpa/inet.h>

#include <postgres.h>
#include <miscadmin.h>

#include <libpq/auth.h>
#include <libpq/libpq.h>
#include <libpq/hba.h>
#include <libpq/password.h>
#include <libpq/crypt.h>


static void sendAuthRequest(Port *port, AuthRequest areq, PacketDoneProc handler);
static int	handle_done_auth(void *arg, PacketLen len, void *pkt);
static int	handle_krb4_auth(void *arg, PacketLen len, void *pkt);
static int	handle_krb5_auth(void *arg, PacketLen len, void *pkt);
static int	handle_password_auth(void *arg, PacketLen len, void *pkt);
static int	readPasswordPacket(void *arg, PacketLen len, void *pkt);
static int	pg_passwordv0_recvauth(void *arg, PacketLen len, void *pkt);
static int	checkPassword(Port *port, char *user, char *password);
static int	old_be_recvauth(Port *port);
static int	map_old_to_new(Port *port, UserAuth old, int status);


#ifdef KRB4
/* This has to be ifdef'd out because krb.h does exist.  This needs
   to be fixed.
*/
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 4
 *----------------------------------------------------------------
 */

#include <krb.h>

/*
 * pg_krb4_recvauth -- server routine to receive authentication information
 *					   from the client
 *
 * Nothing unusual here, except that we compare the username obtained from
 * the client's setup packet to the authenticated name.  (We have to retain
 * the name in the setup packet since we have to retain the ability to handle
 * unauthenticated connections.)
 */
static int
pg_krb4_recvauth(Port *port)
{
	long					krbopts = 0;	/* one-way authentication */
	KTEXT_ST			clttkt;
	char					instance[INST_SZ],
								version[KRB_SENDAUTH_VLEN];
	AUTH_DAT			auth_data;
	Key_schedule	key_sched;
	int						status;

	strcpy(instance, "*");		/* don't care, but arg gets expanded
								 * anyway */
	status = krb_recvauth(krbopts,
						  port->sock,
						  &clttkt,
						  PG_KRB_SRVNAM,
						  instance,
						  &port->raddr.in,
						  &port->laddr.in,
						  &auth_data,
						  PG_KRB_SRVTAB,
						  key_sched,
						  version);
	if (status != KSUCCESS)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_krb4_recvauth: kerberos error: %s\n", krb_err_txt[status]);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	if (strncmp(version, PG_KRB4_VERSION, KRB_SENDAUTH_VLEN))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_krb4_recvauth: protocol version != \"%s\"\n", PG_KRB4_VERSION);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	if (strncmp(port->user, auth_data.pname, SM_USER))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_krb4_recvauth: name \"%s\" != \"%s\"\n",
				port->user, auth_data.pname);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

#else
static int
pg_krb4_recvauth(Port *port)
{
	snprintf(PQerrormsg, ERROR_MSG_LENGTH,
			"pg_krb4_recvauth: Kerberos not implemented on this server.\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	return STATUS_ERROR;
}

#endif	 /* KRB4 */


#ifdef KRB5
/* This needs to be ifdef'd out because krb5.h doesn't exist.  This needs
   to be fixed.
*/
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 5
 *----------------------------------------------------------------
 */

#include <krb5/krb5.h>

/*
 * pg_an_to_ln -- return the local name corresponding to an authentication
 *				  name
 *
 * XXX Assumes that the first aname component is the user name.  This is NOT
 *	   necessarily so, since an aname can actually be something out of your
 *	   worst X.400 nightmare, like
 *		  ORGANIZATION=U. C. Berkeley/NAME=Paul M. Aoki@CS.BERKELEY.EDU
 *	   Note that the MIT an_to_ln code does the same thing if you don't
 *	   provide an aname mapping database...it may be a better idea to use
 *	   krb5_an_to_ln, except that it punts if multiple components are found,
 *	   and we can't afford to punt.
 */
static char *
pg_an_to_ln(char *aname)
{
	char	   *p;

	if ((p = strchr(aname, '/')) || (p = strchr(aname, '@')))
		*p = '\0';
	return aname;
}

/*
 * pg_krb5_recvauth -- server routine to receive authentication information
 *					   from the client
 *
 * We still need to compare the username obtained from the client's setup
 * packet to the authenticated name, as described in pg_krb4_recvauth.	This
 * is a bit more problematic in v5, as described above in pg_an_to_ln.
 *
 * In addition, as described above in pg_krb5_sendauth, we still need to
 * canonicalize the server name v4-style before constructing a principal
 * from it.  Again, this is kind of iffy.
 *
 * Finally, we need to tangle with the fact that v5 doesn't let you explicitly
 * set server keytab file names -- you have to feed lower-level routines a
 * function to retrieve the contents of a keytab, along with a single argument
 * that allows them to open the keytab.  We assume that a server keytab is
 * always a real file so we can allow people to specify their own filenames.
 * (This is important because the POSTGRES keytab needs to be readable by
 * non-root users/groups; the v4 tools used to force you do dump a whole
 * host's worth of keys into a file, effectively forcing you to use one file,
 * but kdb5_edit allows you to select which principals to dump.  Yay!)
 */
static int
pg_krb5_recvauth(Port *port)
{
	char		servbuf[MAXHOSTNAMELEN + 1 +
									sizeof(PG_KRB_SRVNAM)];
	char	   *hostp,
			   *kusername = (char *) NULL;
	krb5_error_code code;
	krb5_principal client,
				server;
	krb5_address sender_addr;
	krb5_rdreq_key_proc keyproc = (krb5_rdreq_key_proc) NULL;
	krb5_pointer keyprocarg = (krb5_pointer) NULL;

	/*
	 * Set up server side -- since we have no ticket file to make this
	 * easy, we construct our own name and parse it.  See note on
	 * canonicalization above.
	 */
	strcpy(servbuf, PG_KRB_SRVNAM);
	*(hostp = servbuf + (sizeof(PG_KRB_SRVNAM) - 1)) = '/';
	if (gethostname(++hostp, MAXHOSTNAMELEN) < 0)
		strcpy(hostp, "localhost");
	if (hostp = strchr(hostp, '.'))
		*hostp = '\0';
	if (code = krb5_parse_name(servbuf, &server))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
			  "pg_krb5_recvauth: Kerberos error %d in krb5_parse_name\n", code);
		com_err("pg_krb5_recvauth", code, "in krb5_parse_name");
		return STATUS_ERROR;
	}

	/*
	 * krb5_sendauth needs this to verify the address in the client
	 * authenticator.
	 */
	sender_addr.addrtype = port->raddr.in.sin_family;
	sender_addr.length = sizeof(port->raddr.in.sin_addr);
	sender_addr.contents = (krb5_octet *) & (port->raddr.in.sin_addr);

	if (strcmp(PG_KRB_SRVTAB, ""))
	{
		keyproc = krb5_kt_read_service_key;
		keyprocarg = PG_KRB_SRVTAB;
	}

	if (code = krb5_recvauth((krb5_pointer) & port->sock,
							 PG_KRB5_VERSION,
							 server,
							 &sender_addr,
							 (krb5_pointer) NULL,
							 keyproc,
							 keyprocarg,
							 (char *) NULL,
							 (krb5_int32 *) NULL,
							 &client,
							 (krb5_ticket **) NULL,
							 (krb5_authenticator **) NULL))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_krb5_recvauth: Kerberos error %d in krb5_recvauth\n", code);
		com_err("pg_krb5_recvauth", code, "in krb5_recvauth");
		krb5_free_principal(server);
		return STATUS_ERROR;
	}
	krb5_free_principal(server);

	/*
	 * The "client" structure comes out of the ticket and is therefore
	 * authenticated.  Use it to check the username obtained from the
	 * postmaster startup packet.
	 */
	if ((code = krb5_unparse_name(client, &kusername)))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
			"pg_krb5_recvauth: Kerberos error %d in krb5_unparse_name\n", code);
		com_err("pg_krb5_recvauth", code, "in krb5_unparse_name");
		krb5_free_principal(client);
		return STATUS_ERROR;
	}
	krb5_free_principal(client);
	if (!kusername)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_krb5_recvauth: could not decode username\n");
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	kusername = pg_an_to_ln(kusername);
	if (strncmp(username, kusername, SM_USER))
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_krb5_recvauth: name \"%s\" != \"%s\"\n", port->user, kusername);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		pfree(kusername);
		return STATUS_ERROR;
	}
	pfree(kusername);
	return STATUS_OK;
}

#else
static int
pg_krb5_recvauth(Port *port)
{
	snprintf(PQerrormsg, ERROR_MSG_LENGTH,
			"pg_krb5_recvauth: Kerberos not implemented on this server.\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	return STATUS_ERROR;
}

#endif	 /* KRB5 */


/*
 * Handle a v0 password packet.
 */

static int
pg_passwordv0_recvauth(void *arg, PacketLen len, void *pkt)
{
	Port	   *port;
	PasswordPacketV0 *pp;
	char	   *user,
			   *password,
			   *cp,
			   *start;

	port = (Port *) arg;
	pp = (PasswordPacketV0 *) pkt;

	/*
	 * The packet is supposed to comprise the user name and the password
	 * as C strings.  Be careful the check that this is the case.
	 */

	user = password = NULL;

	len -= sizeof(pp->unused);

	cp = start = pp->data;

	while (len-- > 0)
		if (*cp++ == '\0')
		{
			if (user == NULL)
				user = start;
			else
			{
				password = start;
				break;
			}

			start = cp;
		}

	if (user == NULL || password == NULL)
	{
		snprintf(PQerrormsg, ERROR_MSG_LENGTH,
				"pg_password_recvauth: badly formed password packet.\n");
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);

		auth_failed(port);
	}
	else
	{
		int			status;
		UserAuth	saved;

		/* Check the password. */

		saved = port->auth_method;
		port->auth_method = uaPassword;

		status = checkPassword(port, user, password);

		port->auth_method = saved;

		/* Adjust the result if necessary. */

		if (map_old_to_new(port, uaPassword, status) != STATUS_OK)
			auth_failed(port);
	}

	return STATUS_OK;			/* don't close the connection yet */
}


/*
 * Tell the user the authentication failed, but not why.
 */

void
auth_failed(Port *port)
{
	PacketSendError(&port->pktInfo, "User authentication failed");
}


/*
 * be_recvauth -- server demux routine for incoming authentication information
 */
void
be_recvauth(Port *port)
{

	/*
	 * Get the authentication method to use for this frontend/database
	 * combination.
	 */

	if (hba_getauthmethod(&port->raddr, port->user, port->database,
						port->auth_arg, &port->auth_method) != STATUS_OK)
		PacketSendError(&port->pktInfo, "Missing or mis-configured pg_hba.conf file");

	else if (PG_PROTOCOL_MAJOR(port->proto) == 0)
	{
		/* Handle old style authentication. */

		if (old_be_recvauth(port) != STATUS_OK)
			auth_failed(port);
	}
	else
	{
		AuthRequest areq;
		PacketDoneProc auth_handler;

		/* Keep the compiler quiet. */

		areq = AUTH_REQ_OK;

		/* Handle new style authentication. */

		auth_handler = NULL;

		switch (port->auth_method)
		{
			case uaReject:
				break;

			case uaKrb4:
				areq = AUTH_REQ_KRB4;
				auth_handler = handle_krb4_auth;
				break;

			case uaKrb5:
				areq = AUTH_REQ_KRB5;
				auth_handler = handle_krb5_auth;
				break;

			case uaTrust:
				areq = AUTH_REQ_OK;
				auth_handler = handle_done_auth;
				break;

			case uaIdent:
				if (authident(&port->raddr.in, &port->laddr.in,
							  port->user, port->auth_arg) == STATUS_OK)
				{
					areq = AUTH_REQ_OK;
					auth_handler = handle_done_auth;
				}

				break;

			case uaPassword:
				areq = AUTH_REQ_PASSWORD;
				auth_handler = handle_password_auth;
				break;

			case uaCrypt:
				areq = AUTH_REQ_CRYPT;
				auth_handler = handle_password_auth;
				break;
		}

		/* Tell the frontend what we want next. */

		if (auth_handler != NULL)
			sendAuthRequest(port, areq, auth_handler);
		else
			auth_failed(port);
	}
}


/*
 * Send an authentication request packet to the frontend.
 */

static void
sendAuthRequest(Port *port, AuthRequest areq, PacketDoneProc handler)
{
	char	   *dp,
			   *sp;
	int			i;
	uint32		net_areq;

	/* Convert to a byte stream. */

	net_areq = htonl(areq);

	dp = port->pktInfo.pkt.ar.data;
	sp = (char *) &net_areq;

	*dp++ = 'R';

	for (i = 1; i <= 4; ++i)
		*dp++ = *sp++;

	/* Add the salt for encrypted passwords. */

	if (areq == AUTH_REQ_CRYPT)
	{
		*dp++ = port->salt[0];
		*dp++ = port->salt[1];
		i += 2;
	}

	PacketSendSetup(&port->pktInfo, i, handler, (void *) port);
}


/*
 * Called when we have told the front end that it is authorised.
 */

static int
handle_done_auth(void *arg, PacketLen len, void *pkt)
{

	/*
	 * Don't generate any more traffic.  This will cause the backend to
	 * start.
	 */

	return STATUS_OK;
}


/*
 * Called when we have told the front end that it should use Kerberos V4
 * authentication.
 */

static int
handle_krb4_auth(void *arg, PacketLen len, void *pkt)
{
	Port	   *port = (Port *) arg;

	if (pg_krb4_recvauth(port) != STATUS_OK)
		auth_failed(port);
	else
		sendAuthRequest(port, AUTH_REQ_OK, handle_done_auth);

	return STATUS_OK;
}


/*
 * Called when we have told the front end that it should use Kerberos V5
 * authentication.
 */

static int
handle_krb5_auth(void *arg, PacketLen len, void *pkt)
{
	Port	   *port = (Port *) arg;

	if (pg_krb5_recvauth(port) != STATUS_OK)
		auth_failed(port);
	else
		sendAuthRequest(port, AUTH_REQ_OK, handle_done_auth);

	return STATUS_OK;
}


/*
 * Called when we have told the front end that it should use password
 * authentication.
 */

static int
handle_password_auth(void *arg, PacketLen len, void *pkt)
{
	Port	   *port = (Port *) arg;

	/* Set up the read of the password packet. */

	PacketReceiveSetup(&port->pktInfo, readPasswordPacket, (void *) port);

	return STATUS_OK;
}


/*
 * Called when we have received the password packet.
 */

static int
readPasswordPacket(void *arg, PacketLen len, void *pkt)
{
	char		password[sizeof(PasswordPacket) + 1];
	Port	   *port = (Port *) arg;

	/* Silently truncate a password that is too big. */

	if (len > sizeof(PasswordPacket))
		len = sizeof(PasswordPacket);

	StrNCpy(password, ((PasswordPacket *) pkt)->passwd, len);

	if (checkPassword(port, port->user, password) != STATUS_OK)
		auth_failed(port);
	else
		sendAuthRequest(port, AUTH_REQ_OK, handle_done_auth);

	return STATUS_OK;			/* don't close the connection yet */
}


/*
 * Use the local flat password file if clear passwords are used and the file is
 * specified.  Otherwise use the password in the pg_shadow table, encrypted or
 * not.
 */

static int
checkPassword(Port *port, char *user, char *password)
{
	if (port->auth_method == uaPassword && port->auth_arg[0] != '\0')
		return verify_password(port->auth_arg, user, password);

	return crypt_verify(port, user, password);
}


/*
 * Server demux routine for incoming authentication information for protocol
 * version 0.
 */
static int
old_be_recvauth(Port *port)
{
	int			status;
	MsgType		msgtype = (MsgType) port->proto;

	/* Handle the authentication that's offered. */

	switch (msgtype)
	{
		case STARTUP_KRB4_MSG:
			status = map_old_to_new(port, uaKrb4, pg_krb4_recvauth(port));
			break;

		case STARTUP_KRB5_MSG:
			status = map_old_to_new(port, uaKrb5, pg_krb5_recvauth(port));
			break;

		case STARTUP_MSG:
			status = map_old_to_new(port, uaTrust, STATUS_OK);
			break;

		case STARTUP_PASSWORD_MSG:
			PacketReceiveSetup(&port->pktInfo, pg_passwordv0_recvauth,
							   (void *) port);

			return STATUS_OK;

		default:
			fprintf(stderr, "Invalid startup message type: %u\n", msgtype);

			return STATUS_OK;
	}

	return status;
}


/*
 * The old style authentication has been done.	Modify the result of this (eg.
 * allow the connection anyway, disallow it anyway, or use the result)
 * depending on what authentication we really want to use.
 */

static int
map_old_to_new(Port *port, UserAuth old, int status)
{
	switch (port->auth_method)
	{
			case uaCrypt:
			case uaReject:
			status = STATUS_ERROR;
			break;

		case uaKrb4:
			if (old != uaKrb4)
				status = STATUS_ERROR;
			break;

		case uaKrb5:
			if (old != uaKrb5)
				status = STATUS_ERROR;
			break;

		case uaTrust:
			status = STATUS_OK;
			break;

		case uaIdent:
			status = authident(&port->raddr.in, &port->laddr.in,
							   port->user, port->auth_arg);
			break;

		case uaPassword:
			if (old != uaPassword)
				status = STATUS_ERROR;

			break;
	}

	return status;
}
