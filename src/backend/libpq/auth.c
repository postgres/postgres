/*-------------------------------------------------------------------------
 *
 * auth.c--
 *    Routines to handle network authentication
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/libpq/auth.c,v 1.13 1997/08/12 22:52:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *     backend (postmaster) routines:
 *	be_recvauth		receive authentication information
 *	be_setauthsvc		do/do not permit an authentication service
 *	be_getauthsvc		is an authentication service permitted?
 *
 *   NOTES
 *	To add a new authentication system:
 *	0. If you can't do your authentication over an existing socket,
 *	   you lose -- get ready to hack around this framework instead of 
 *	   using it.  Otherwise, you can assume you have an initialized
 *	   and empty connection to work with.  (Please don't leave leftover
 *	   gunk in the connection after the authentication transactions, or
 *	   the POSTGRES routines that follow will be very unhappy.)
 *	1. Write a set of routines that:
 *		let a client figure out what user/principal name to use
 *		send authentication information (client side)
 *		receive authentication information (server side)
 *	   You can include both routines in this file, using #ifdef FRONTEND
 *	   to separate them.
 *	2. Edit libpq/pqcomm.h and assign a MsgType for your protocol.
 *	3. Edit the static "struct authsvc" array and the generic 
 *	   {be,fe}_{get,set}auth{name,svc} routines in this file to reflect 
 *	   the new service.  You may have to change the arguments of these
 *	   routines; they basically just reflect what Kerberos v4 needs.
 *	4. Hack on src/{,bin}/Makefile.global and src/{backend,libpq}/Makefile
 *	   to add library and CFLAGS hooks -- basically, grep the Makefile
 *	   hierarchy for KRBVERS to see where you need to add things.
 *
 *	Send mail to post_hackers@postgres.Berkeley.EDU if you have to make 
 *	any changes to arguments, etc.  Context diffs would be nice, too.
 *
 *	Someday, this cruft will go away and magically be replaced by a
 *	nice interface based on the GSS API or something.  For now, though,
 *	there's no (stable) UNIX security API to work with...
 *
 */
#include <stdio.h>
#include <string.h>
#include <sys/param.h>	/* for MAXHOSTNAMELEN on most */
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>	/* for MAXHOSTNAMELEN on some */
#endif
#include <pwd.h>
#include <ctype.h>		        /* isspace() declaration */

#include <sys/types.h>    /* needed by in.h on Ultrix */
#include <netinet/in.h>
#include <arpa/inet.h>

#include <postgres.h>
#include <miscadmin.h>

#include <libpq/auth.h>
#include <libpq/libpq.h>
#include <libpq/libpq-be.h>
#include <libpq/hba.h>
#include <libpq/password.h>

/*----------------------------------------------------------------
 * common definitions for generic fe/be routines
 *----------------------------------------------------------------
 */

struct authsvc {
    char	name[16];	/* service nickname (for command line) */
    MsgType	msgtype;	/* startup packet header type */
    int		allowed;	/* initially allowed (before command line
				 * option parsing)?
				 */
};

/*
 * Command-line parsing routines use this structure to map nicknames
 * onto service types (and the startup packets to use with them).
 *
 * Programs receiving an authentication request use this structure to
 * decide which authentication service types are currently permitted.
 * By default, all authentication systems compiled into the system are
 * allowed.  Unauthenticated connections are disallowed unless there
 * isn't any authentication system.
 */

#if defined(HBA)
static int useHostBasedAuth = 1;
#else
static int useHostBasedAuth = 0;
#endif

#if defined(KRB4) || defined(KRB5) || defined(HBA)
#define UNAUTH_ALLOWED 0
#else
#define UNAUTH_ALLOWED 1
#endif

static struct authsvc authsvcs[] = {
    { "unauth",   STARTUP_UNAUTH_MSG, UNAUTH_ALLOWED },
    { "hba",      STARTUP_HBA_MSG,  1 },
    { "krb4",     STARTUP_KRB4_MSG, 1 },
    { "krb5",     STARTUP_KRB5_MSG, 1 },
#if defined(KRB5) 
    { "kerberos", STARTUP_KRB5_MSG, 1 },
#else
    { "kerberos", STARTUP_KRB4_MSG, 1 },
#endif
    { "password", STARTUP_PASSWORD_MSG, 1 }
};

static n_authsvcs = sizeof(authsvcs) / sizeof(struct authsvc);

#ifdef KRB4
/* This has to be ifdef'd out because krb.h does exist.  This needs
   to be fixed.
*/
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 4
 *----------------------------------------------------------------
 */

#include <krb.h>

#ifdef FRONTEND
/* moves to src/libpq/fe-auth.c  */
#else /* !FRONTEND */

/*
 * pg_krb4_recvauth -- server routine to receive authentication information
 *		       from the client
 *
 * Nothing unusual here, except that we compare the username obtained from
 * the client's setup packet to the authenticated name.  (We have to retain
 * the name in the setup packet since we have to retain the ability to handle
 * unauthenticated connections.)
 */
static int
pg_krb4_recvauth(int sock,
		 struct sockaddr_in *laddr,
		 struct sockaddr_in *raddr,
		 char *username)
{
    long		krbopts = 0;	/* one-way authentication */
    KTEXT_ST	clttkt;
    char		instance[INST_SZ];
    AUTH_DAT	auth_data;
    Key_schedule	key_sched;
    char		version[KRB_SENDAUTH_VLEN];
    int		status;
    
    strcpy(instance, "*");	/* don't care, but arg gets expanded anyway */
    status = krb_recvauth(krbopts,
			  sock,
			  &clttkt,
			  PG_KRB_SRVNAM,
			  instance,
			  raddr,
			  laddr,
			  &auth_data,
			  PG_KRB_SRVTAB,
			  key_sched,
			  version);
    if (status != KSUCCESS) {
	sprintf(PQerrormsg,
		       "pg_krb4_recvauth: kerberos error: %s\n",
		       krb_err_txt[status]);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    if (strncmp(version, PG_KRB4_VERSION, KRB_SENDAUTH_VLEN)) {
	sprintf(PQerrormsg,
		       "pg_krb4_recvauth: protocol version != \"%s\"\n",
		       PG_KRB4_VERSION);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    if (username && *username &&
	strncmp(username, auth_data.pname, NAMEDATALEN)) {
	sprintf(PQerrormsg,
		       "pg_krb4_recvauth: name \"%s\" != \"%s\"\n",
		       username,
		       auth_data.pname);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    return(STATUS_OK);
}

#endif /* !FRONTEND */

#else
static int
pg_krb4_recvauth(int sock,
		 struct sockaddr_in *laddr,
		 struct sockaddr_in *raddr,
		 char *username)
{
  sprintf(PQerrormsg,
                 "pg_krb4_recvauth: Kerberos not implemented on this "
                 "server.\n");
  fputs(PQerrormsg, stderr);
  pqdebug("%s", PQerrormsg);

return(STATUS_ERROR);
}
#endif /* KRB4 */


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
 *		  name
 *
 * XXX Assumes that the first aname component is the user name.  This is NOT
 *     necessarily so, since an aname can actually be something out of your
 *     worst X.400 nightmare, like
 *	  ORGANIZATION=U. C. Berkeley/NAME=Paul M. Aoki@CS.BERKELEY.EDU
 *     Note that the MIT an_to_ln code does the same thing if you don't
 *     provide an aname mapping database...it may be a better idea to use
 *     krb5_an_to_ln, except that it punts if multiple components are found,
 *     and we can't afford to punt.
 */
static char *
pg_an_to_ln(char *aname)
{
    char	*p;
    
    if ((p = strchr(aname, '/')) || (p = strchr(aname, '@')))
	*p = '\0';
    return(aname);
}

#ifdef FRONTEND
/* moves to src/libpq/fe-auth.c  */
#else /* !FRONTEND */

/*
 * pg_krb5_recvauth -- server routine to receive authentication information
 *		       from the client
 *
 * We still need to compare the username obtained from the client's setup
 * packet to the authenticated name, as described in pg_krb4_recvauth.  This
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
pg_krb5_recvauth(int sock,
		 struct sockaddr_in *laddr,
		 struct sockaddr_in *raddr,
		 char *username)
{
    char			servbuf[MAXHOSTNAMELEN + 1 +
					sizeof(PG_KRB_SRVNAM)];
    char			*hostp, *kusername = (char *) NULL;
    krb5_error_code		code;
    krb5_principal		client, server;
    krb5_address		sender_addr;
    krb5_rdreq_key_proc	keyproc = (krb5_rdreq_key_proc) NULL;
    krb5_pointer		keyprocarg = (krb5_pointer) NULL;
    
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
    if (code = krb5_parse_name(servbuf, &server)) {
	sprintf(PQerrormsg,
		       "pg_krb5_recvauth: Kerberos error %d in krb5_parse_name\n",
		       code);
	com_err("pg_krb5_recvauth", code, "in krb5_parse_name");
	return(STATUS_ERROR);
    }
    
    /*
     * krb5_sendauth needs this to verify the address in the client
     * authenticator.
     */
    sender_addr.addrtype = raddr->sin_family;
    sender_addr.length = sizeof(raddr->sin_addr);
    sender_addr.contents = (krb5_octet *) &(raddr->sin_addr);
    
    if (strcmp(PG_KRB_SRVTAB, "")) {
	keyproc = krb5_kt_read_service_key;
	keyprocarg = PG_KRB_SRVTAB;
    }
    
    if (code = krb5_recvauth((krb5_pointer) &sock,
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
			     (krb5_authenticator **) NULL)) {
	sprintf(PQerrormsg,
		       "pg_krb5_recvauth: Kerberos error %d in krb5_recvauth\n",
		       code);
	com_err("pg_krb5_recvauth", code, "in krb5_recvauth");
	krb5_free_principal(server);
	return(STATUS_ERROR);
    }
    krb5_free_principal(server);
    
    /*
     * The "client" structure comes out of the ticket and is therefore
     * authenticated.  Use it to check the username obtained from the
     * postmaster startup packet.
     */
    if ((code = krb5_unparse_name(client, &kusername))) {
	sprintf(PQerrormsg,
		       "pg_krb5_recvauth: Kerberos error %d in krb5_unparse_name\n",
		       code);
	com_err("pg_krb5_recvauth", code, "in krb5_unparse_name");
	krb5_free_principal(client);
	return(STATUS_ERROR);
    }
    krb5_free_principal(client);
    if (!kusername) {
	sprintf(PQerrormsg,
		       "pg_krb5_recvauth: could not decode username\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    kusername = pg_an_to_ln(kusername);
    if (username && strncmp(username, kusername, NAMEDATALEN)) {
	sprintf(PQerrormsg,
		       "pg_krb5_recvauth: name \"%s\" != \"%s\"\n",
		       username, kusername);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	free(kusername);
	return(STATUS_ERROR);
    }
    free(kusername);
    return(STATUS_OK);
}

#endif /* !FRONTEND */


#else
static int
pg_krb5_recvauth(int sock,
		 struct sockaddr_in *laddr,
		 struct sockaddr_in *raddr,
		 char *username)
{
  sprintf(PQerrormsg,
                 "pg_krb5_recvauth: Kerberos not implemented on this "
                 "server.\n");
  fputs(PQerrormsg, stderr);
  pqdebug("%s", PQerrormsg);

return(STATUS_ERROR);
}
#endif /* KRB5 */

static int
pg_password_recvauth(Port *port, char *database, char *DataDir)
{
    PacketBuf buf;
    char *user, *password;

    if(PacketReceive(port, &buf, BLOCKING) != STATUS_OK) {
	sprintf(PQerrormsg,
		"pg_password_recvauth: failed to receive authentication packet.\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return STATUS_ERROR;
    }

    user = buf.data;
    password = buf.data + strlen(user) + 1;

    return verify_password(user, password, port, database, DataDir);
}

/*
 * be_recvauth -- server demux routine for incoming authentication information
 */
int
be_recvauth(MsgType msgtype_arg, Port *port, char *username, StartupInfo* sp)
{
    MsgType msgtype;

    /* A message type of STARTUP_MSG (which once upon a time was the only
       startup message type) means user wants us to choose.  "unauth" is 
       what used to be the only choice, but installation may choose "hba"
       instead.
       */
    if (msgtype_arg == STARTUP_MSG) {
       if(useHostBasedAuth)
           msgtype = STARTUP_HBA_MSG;
       else
           msgtype = STARTUP_UNAUTH_MSG;
    } else
        msgtype = msgtype_arg;


    if (!username) {
        sprintf(PQerrormsg,
                       "be_recvauth: no user name passed\n");
        fputs(PQerrormsg, stderr);
        pqdebug("%s", PQerrormsg);
        return(STATUS_ERROR);
    }
    if (!port) {
        sprintf(PQerrormsg,
        "be_recvauth: no port structure passed\n");
        fputs(PQerrormsg, stderr);
        pqdebug("%s", PQerrormsg);
        return(STATUS_ERROR);
    }
    
    switch (msgtype) {
    case STARTUP_KRB4_MSG:
        if (!be_getauthsvc(msgtype)) {
            sprintf(PQerrormsg,
                           "be_recvauth: krb4 authentication disallowed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
        }
	if (pg_krb4_recvauth(port->sock, &port->laddr, &port->raddr,
                             username) != STATUS_OK) {
            sprintf(PQerrormsg,
                           "be_recvauth: krb4 authentication failed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
        }
	break;
    case STARTUP_KRB5_MSG:
        if (!be_getauthsvc(msgtype)) {
            sprintf(PQerrormsg,
                           "be_recvauth: krb5 authentication disallowed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
          }
        if (pg_krb5_recvauth(port->sock, &port->laddr, &port->raddr,
                             username) != STATUS_OK) {
            sprintf(PQerrormsg,
                           "be_recvauth: krb5 authentication failed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
        }
        break;
    case STARTUP_UNAUTH_MSG:
        if (!be_getauthsvc(msgtype)) {
            sprintf(PQerrormsg,
                           "be_recvauth: "
                           "unauthenticated connections disallowed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
        }
        break;
    case STARTUP_HBA_MSG:
        if (hba_recvauth(port, sp->database, sp->user, DataDir) != STATUS_OK) {
            sprintf(PQerrormsg,
                           "be_recvauth: host-based authentication failed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
          }
        break;
    case STARTUP_PASSWORD_MSG:
        if(!be_getauthsvc(msgtype)) {
	    sprintf(PQerrormsg, 
		    "be_recvauth: "
		    "plaintext password authentication disallowed\n");
            fputs(PQerrormsg, stderr);
            pqdebug("%s", PQerrormsg);
            return(STATUS_ERROR);
	}
	if(pg_password_recvauth(port, sp->database, DataDir) != STATUS_OK) {
	    /* pg_password_recvauth or lower-level routines have already set */
  	    /* the error message                                             */
            return(STATUS_ERROR);
	}
	break;
    default:
        sprintf(PQerrormsg,
                       "be_recvauth: unrecognized message type: %d\n",
                       msgtype);
        fputs(PQerrormsg, stderr);
        pqdebug("%s", PQerrormsg);
        return(STATUS_ERROR);
    }
    return(STATUS_OK);
}

/*
 * be_setauthsvc -- enable/disable the authentication services currently
 *		    selected for use by the backend
 * be_getauthsvc -- returns whether a particular authentication system
 *		    (indicated by its message type) is permitted by the
 *		    current selections
 *
 * be_setauthsvc encodes the command-line syntax that
 *	-a "<service-name>"
 * enables a service, whereas
 *	-a "no<service-name>"
 * disables it.
 */
void
be_setauthsvc(char *name)
{
    int i, j;
    int turnon = 1;
    
    if (!name)
	return;
    if (!strncmp("no", name, 2)) {
	turnon = 0;
	name += 2;
    }
    if (name[0] == '\0')
	return;
    for (i = 0; i < n_authsvcs; ++i)
	if (!strcmp(name, authsvcs[i].name)) {
	    for (j = 0; j < n_authsvcs; ++j)
		if (authsvcs[j].msgtype == authsvcs[i].msgtype)
		    authsvcs[j].allowed = turnon;
	    break;
	}
    if (i == n_authsvcs) {
	sprintf(PQerrormsg,
		       "be_setauthsvc: invalid name %s, ignoring...\n",
		       name);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
    }
    return;
}

int
be_getauthsvc(MsgType msgtype)
{
    int i;
    
    for (i = 0; i < n_authsvcs; ++i)
	if (msgtype == authsvcs[i].msgtype)
	    return(authsvcs[i].allowed);
    return(0);
}
