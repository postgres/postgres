/*-------------------------------------------------------------------------
 *
 * auth.c
 *	  Routines to handle network authentication
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/auth.c,v 1.55 2001/08/01 23:25:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/types.h>			/* needed by in.h on Ultrix */
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "libpq/password.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"

static void sendAuthRequest(Port *port, AuthRequest areq);

static int	checkPassword(Port *port, char *user, char *password);
static int	old_be_recvauth(Port *port);
static int	map_old_to_new(Port *port, UserAuth old, int status);
static void auth_failed(Port *port);

static int	recv_and_check_password_packet(Port *port);
static int	recv_and_check_passwordv0(Port *port);

char	   *pg_krb_server_keyfile;


#ifdef KRB4
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 4
 *----------------------------------------------------------------
 */

#include "krb.h"

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
	long		krbopts = 0;	/* one-way authentication */
	KTEXT_ST	clttkt;
	char		instance[INST_SZ + 1],
				version[KRB_SENDAUTH_VLEN + 1];
	AUTH_DAT	auth_data;
	Key_schedule key_sched;
	int			status;

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
						  pg_krb_server_keyfile,
						  key_sched,
						  version);
	if (status != KSUCCESS)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb4_recvauth: kerberos error: %s\n",
				 krb_err_txt[status]);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	if (strncmp(version, PG_KRB4_VERSION, KRB_SENDAUTH_VLEN))
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb4_recvauth: protocol version != \"%s\"\n",
				 PG_KRB4_VERSION);
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);
		return STATUS_ERROR;
	}
	if (strncmp(port->user, auth_data.pname, SM_USER))
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
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
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
		 "pg_krb4_recvauth: Kerberos not implemented on this server.\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);

	return STATUS_ERROR;
}
#endif	 /* KRB4 */


#ifdef KRB5
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 5
 *----------------------------------------------------------------
 */

#include <krb5.h>
#include <com_err.h>

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
 * Various krb5 state which is not connection specfic, and a flag to
 * indicate whether we have initialised it yet.
 */
static int	pg_krb5_initialised;
static krb5_context pg_krb5_context;
static krb5_keytab pg_krb5_keytab;
static krb5_principal pg_krb5_server;


static int
pg_krb5_init(void)
{
	krb5_error_code retval;

	if (pg_krb5_initialised)
		return STATUS_OK;

	retval = krb5_init_context(&pg_krb5_context);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_init_context returned"
				 " Kerberos error %d\n", retval);
		com_err("postgres", retval, "while initializing krb5");
		return STATUS_ERROR;
	}

	retval = krb5_kt_resolve(pg_krb5_context, pg_krb_server_keyfile, &pg_krb5_keytab);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_kt_resolve returned"
				 " Kerberos error %d\n", retval);
		com_err("postgres", retval, "while resolving keytab file %s",
				pg_krb_server_keyfile);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}

	retval = krb5_sname_to_principal(pg_krb5_context, NULL, PG_KRB_SRVNAM,
									 KRB5_NT_SRV_HST, &pg_krb5_server);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_init: krb5_sname_to_principal returned"
				 " Kerberos error %d\n", retval);
		com_err("postgres", retval,
				"while getting server principal for service %s",
				pg_krb_server_keyfile);
		krb5_kt_close(pg_krb5_context, pg_krb5_keytab);
		krb5_free_context(pg_krb5_context);
		return STATUS_ERROR;
	}

	pg_krb5_initialised = 1;
	return STATUS_OK;
}


/*
 * pg_krb5_recvauth -- server routine to receive authentication information
 *					   from the client
 *
 * We still need to compare the username obtained from the client's setup
 * packet to the authenticated name, as described in pg_krb4_recvauth.	This
 * is a bit more problematic in v5, as described above in pg_an_to_ln.
 *
 * We have our own keytab file because postgres is unlikely to run as root,
 * and so cannot read the default keytab.
 */
static int
pg_krb5_recvauth(Port *port)
{
	krb5_error_code retval;
	int			ret;
	krb5_auth_context auth_context = NULL;
	krb5_ticket *ticket;
	char	   *kusername;

	ret = pg_krb5_init();
	if (ret != STATUS_OK)
		return ret;

	retval = krb5_recvauth(pg_krb5_context, &auth_context,
						   (krb5_pointer) & port->sock, PG_KRB_SRVNAM,
						   pg_krb5_server, 0, pg_krb5_keytab, &ticket);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_recvauth: krb5_recvauth returned"
				 " Kerberos error %d\n", retval);
		com_err("postgres", retval, "from krb5_recvauth");
		return STATUS_ERROR;
	}

	/*
	 * The "client" structure comes out of the ticket and is therefore
	 * authenticated.  Use it to check the username obtained from the
	 * postmaster startup packet.
	 *
	 * I have no idea why this is considered necessary.
	 */
	retval = krb5_unparse_name(pg_krb5_context,
							   ticket->enc_part2->client, &kusername);
	if (retval)
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_krb5_recvauth: krb5_unparse_name returned"
				 " Kerberos error %d\n", retval);
		com_err("postgres", retval, "while unparsing client name");
		krb5_free_ticket(pg_krb5_context, ticket);
		krb5_auth_con_free(pg_krb5_context, auth_context);
		return STATUS_ERROR;
	}

	kusername = pg_an_to_ln(kusername);
	if (strncmp(port->user, kusername, SM_USER))
	{
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
			  "pg_krb5_recvauth: user name \"%s\" != krb5 name \"%s\"\n",
				 port->user, kusername);
		ret = STATUS_ERROR;
	}
	else
		ret = STATUS_OK;

	krb5_free_ticket(pg_krb5_context, ticket);
	krb5_auth_con_free(pg_krb5_context, auth_context);
	free(kusername);

	return ret;
}

#else
static int
pg_krb5_recvauth(Port *port)
{
	snprintf(PQerrormsg, PQERRORMSG_LENGTH,
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
recv_and_check_passwordv0(Port *port)
{
	int32		len;
	char	   *buf;
	PasswordPacketV0 *pp;
	char	   *user,
			   *password,
			   *cp,
			   *start;

	pq_getint(&len, 4);
	len -= 4;
	buf = palloc(len);
	pq_getbytes(buf, len);

	pp = (PasswordPacketV0 *) buf;

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
		snprintf(PQerrormsg, PQERRORMSG_LENGTH,
				 "pg_password_recvauth: badly formed password packet.\n");
		fputs(PQerrormsg, stderr);
		pqdebug("%s", PQerrormsg);

		pfree(buf);
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

		pfree(buf);
		port->auth_method = saved;

		/* Adjust the result if necessary. */
		if (map_old_to_new(port, uaPassword, status) != STATUS_OK)
			auth_failed(port);
	}

	return STATUS_OK;
}


/*
 * Tell the user the authentication failed, but not (much about) why.
 *
 * There is a tradeoff here between security concerns and making life
 * unnecessarily difficult for legitimate users.  We would not, for example,
 * want to report the password we were expecting to receive...
 * But it seems useful to report the username and authorization method
 * in use, and these are items that must be presumed known to an attacker
 * anyway.
 * Note that many sorts of failure report additional information in the
 * postmaster log, which we hope is only readable by good guys.
 */
static void
auth_failed(Port *port)
{
	const char *authmethod = "Unknown auth method:";

	switch (port->auth_method)
	{
		case uaReject:
			authmethod = "Rejected host:";
			break;
		case uaKrb4:
			authmethod = "Kerberos4";
			break;
		case uaKrb5:
			authmethod = "Kerberos5";
			break;
		case uaTrust:
			authmethod = "Trusted";
			break;
		case uaIdent:
			authmethod = "IDENT";
			break;
		case uaPassword:
			authmethod = "Password";
			break;
		case uaCrypt:
			authmethod = "Password";
			break;
	}

	elog(FATAL, "%s authentication failed for user \"%s\"",
		 authmethod, port->user);
}


/*
 * Client authentication starts here.  If there is an error, this
 * function does not return and the backend process is terminated.
 */
void
ClientAuthentication(Port *port)
{
	int status = STATUS_ERROR;

	/*
	 * Get the authentication method to use for this frontend/database
	 * combination.  Note: a failure return indicates a problem with the
	 * hba config file, not with the request.  hba.c should have dropped
	 * an error message into the postmaster logfile if it failed.
	 */
	if (hba_getauthmethod(port) != STATUS_OK)
		elog(FATAL, "Missing or erroneous pg_hba.conf file, see postmaster log for details");

	/* Handle old style authentication. */
	else if (PG_PROTOCOL_MAJOR(port->proto) == 0)
	{
		if (old_be_recvauth(port) != STATUS_OK)
			auth_failed(port);
		return;
	}

	/* Handle new style authentication. */
	switch (port->auth_method)
	{
		case uaReject:
		/*
		 * This could have come from an explicit "reject" entry in
		 * pg_hba.conf, but more likely it means there was no
		 * matching entry.	Take pity on the poor user and issue a
		 * helpful error message.  NOTE: this is not a security
		 * breach, because all the info reported here is known at
		 * the frontend and must be assumed known to bad guys.
		 * We're merely helping out the less clueful good guys.
		 */
		{
			const char *hostinfo = "localhost";

			if (port->raddr.sa.sa_family == AF_INET)
				hostinfo = inet_ntoa(port->raddr.in.sin_addr);
			elog(FATAL,
				 "No pg_hba.conf entry for host %s, user %s, database %s",
				 hostinfo, port->user, port->database);
			return;
		}
		break;

		case uaKrb4:
			sendAuthRequest(port, AUTH_REQ_KRB4);
			status = pg_krb4_recvauth(port);
			break;

		case uaKrb5:
			sendAuthRequest(port, AUTH_REQ_KRB5);
			status = pg_krb5_recvauth(port);
			break;

		case uaIdent:
			status = authident(port);
			break;

		case uaPassword:
			sendAuthRequest(port, AUTH_REQ_PASSWORD);
			status = recv_and_check_password_packet(port);
			break;

		case uaCrypt:
			sendAuthRequest(port, AUTH_REQ_CRYPT);
			status = recv_and_check_password_packet(port);
			break;

		case uaTrust:
			status = STATUS_OK;
			break;
	}

	if (status == STATUS_OK)
		sendAuthRequest(port, AUTH_REQ_OK);
	else
		auth_failed(port);
}


/*
 * Send an authentication request packet to the frontend.
 */
static void
sendAuthRequest(Port *port, AuthRequest areq)
{
	StringInfoData buf;

	pq_beginmessage(&buf);
	pq_sendbyte(&buf, 'R');
	pq_sendint(&buf, (int32) areq, sizeof(int32));

	/* Add the salt for encrypted passwords. */
	if (areq == AUTH_REQ_CRYPT)
	{
		pq_sendint(&buf, port->salt[0], 1);
		pq_sendint(&buf, port->salt[1], 1);
	}

	pq_endmessage(&buf);
	pq_flush();
}



/*
 * Called when we have received the password packet.
 */
static int
recv_and_check_password_packet(Port *port)
{
	StringInfoData buf;
	int32		len;
	int			result;

	if (pq_getint(&len, 4) == EOF)
		return STATUS_ERROR;	/* client didn't want to send password */
	initStringInfo(&buf);
	pq_getstr(&buf);

	if (DebugLvl)
		fprintf(stderr, "received password packet with len=%d, pw=%s\n",
				len, buf.data);

	result = checkPassword(port, port->user, buf.data);
	pfree(buf.data);
	return result;
}


/*
 * Handle `password' and `crypt' records. If an auth argument was
 * specified, use the respective file. Else use pg_shadow passwords.
 */
static int
checkPassword(Port *port, char *user, char *password)
{
	if (port->auth_arg[0] != '\0')
		return verify_password(port, user, password);

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
			status = recv_and_check_passwordv0(port);
			break;

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
			status = authident(port);
			break;

		case uaPassword:
			if (old != uaPassword)
				status = STATUS_ERROR;

			break;
	}

	return status;
}
