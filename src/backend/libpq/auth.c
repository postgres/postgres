/*-------------------------------------------------------------------------
 *
 * auth.c--
 *    Routines to handle network authentication
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/libpq/auth.c,v 1.1.1.1.2.1 1996/08/26 20:34:49 scrappy Exp $
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
#include <sys/param.h>	/* for MAX{HOSTNAME,PATH}LEN, NOFILE */
#include <pwd.h>
#include <ctype.h>		        /* isspace() declaration */

#include <netinet/in.h>
#include <arpa/inet.h>
#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/pqcomm.h"
#include "libpq/libpq-be.h"

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
static struct authsvc authsvcs[] = {
#ifdef KRB4
    { "krb4",     STARTUP_KRB4_MSG, 1 },
    { "kerberos", STARTUP_KRB4_MSG, 1 },
#endif /* KRB4 */
#ifdef KRB5
    { "krb5",     STARTUP_KRB5_MSG, 1 },
    { "kerberos", STARTUP_KRB5_MSG, 1 },
#endif /* KRB5 */
    { UNAUTHNAME, STARTUP_MSG,
#if defined(KRB4) || defined(KRB5)
	  0
#else /* !(KRB4 || KRB5) */
	  1
#endif /* !(KRB4 || KRB5) */
    }
};

static n_authsvcs = sizeof(authsvcs) / sizeof(struct authsvc);

#ifdef KRB4
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 4
 *----------------------------------------------------------------
 */

#include "krb.h"

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
	(void) sprintf(PQerrormsg,
		       "pg_krb4_recvauth: kerberos error: %s\n",
		       krb_err_txt[status]);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    if (strncmp(version, PG_KRB4_VERSION, KRB_SENDAUTH_VLEN)) {
	(void) sprintf(PQerrormsg,
		       "pg_krb4_recvauth: protocol version != \"%s\"\n",
		       PG_KRB4_VERSION);
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    if (username && *username &&
	strncmp(username, auth_data.pname, NAMEDATALEN)) {
	(void) sprintf(PQerrormsg,
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

#endif /* KRB4 */

#ifdef KRB5
/*----------------------------------------------------------------
 * MIT Kerberos authentication system - protocol version 5
 *----------------------------------------------------------------
 */

#include "krb5/krb5.h"

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
 * pg_krb4_recvauth -- server routine to receive authentication information
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
    (void) strcpy(servbuf, PG_KRB_SRVNAM);
    *(hostp = servbuf + (sizeof(PG_KRB_SRVNAM) - 1)) = '/';
    if (gethostname(++hostp, MAXHOSTNAMELEN) < 0)
	(void) strcpy(hostp, "localhost");
    if (hostp = strchr(hostp, '.'))
	*hostp = '\0';
    if (code = krb5_parse_name(servbuf, &server)) {
	(void) sprintf(PQerrormsg,
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
	(void) sprintf(PQerrormsg,
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
	(void) sprintf(PQerrormsg,
		       "pg_krb5_recvauth: Kerberos error %d in krb5_unparse_name\n",
		       code);
	com_err("pg_krb5_recvauth", code, "in krb5_unparse_name");
	krb5_free_principal(client);
	return(STATUS_ERROR);
    }
    krb5_free_principal(client);
    if (!kusername) {
	(void) sprintf(PQerrormsg,
		       "pg_krb5_recvauth: could not decode username\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    kusername = pg_an_to_ln(kusername);
    if (username && strncmp(username, kusername, NAMEDATALEN)) {
	(void) sprintf(PQerrormsg,
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

#endif /* KRB5 */


/*----------------------------------------------------------------
 * host based authentication
 *----------------------------------------------------------------
 * based on the securelib package originally written by William
 * LeFebvre, EECS Department, Northwestern University
 * (phil@eecs.nwu.edu) - orginal configuration file code handling
 * by Sam Horrocks (sam@ics.uci.edu)
 *
 * modified and adapted for use with Postgres95 by Paul Fisher
 * (pnfisher@unity.ncsu.edu)
 */

#define CONF_FILE "pg_hba"              /* Name of the config file           */

#define MAX_LINES 255                    /* Maximum number of config lines    *
                                         * that can apply to one database    */

#define ALL_NAME "all"                  /* Name used in config file for      *
                                         * lines that apply to all databases */

#define MAX_TOKEN 80                    /* Maximum size of one token in the  *
                                         * configuration file                */
 
struct conf_line {                      /* Info about config file line */
  u_long adr, mask;
};
 
static int next_token(FILE *, char *, int);

/* hba_recvauth */
/* check for host-based authentication */
/*
 * hba_recvauth - check the sockaddr_in "addr" to see if it corresponds
 *                to an acceptable host for the database that's being
 *                connected to.  Return STATUS_OK if acceptable,
 *                otherwise return STATUS_ERROR.
 */

static int
hba_recvauth(struct sockaddr_in *addr, PacketBuf *pbuf, StartupInfo *sp)
{
    u_long ip_addr;
    static struct conf_line conf[MAX_LINES];
    static int nconf;
    int i;

    char buf[MAX_TOKEN];
    FILE *file;

    char *conf_file;

    /* put together the full pathname to the config file */
    conf_file = (char *) malloc((strlen(DataDir)+strlen(CONF_FILE)+2)*sizeof(char));
    sprintf(conf_file, "%s/%s", DataDir, CONF_FILE);

    /* Open the config file. */
    file = fopen(conf_file, "r");
    if (file)
    {
        free(conf_file);
	nconf = 0;

	/* Grab the "name" */
	while ((i = next_token(file, buf, sizeof(buf))) != EOF)
	{
	    /* If only token on the line, ignore */
	    if (i == '\n') continue;
	    
	    /* Comment -- read until end of line then next line */
	    if (buf[0] == '#')
	    {
	        while (next_token(file, buf, sizeof(buf)) == 0) ;
	        continue;
	    }

	    /*
	     * Check to make sure this says "all" or that it matches
	     * the database name.
	     */
	    
	    if (strcmp(buf, ALL_NAME) == 0 || (strcmp(buf, sp->database) == 0))
	    {
	        /* Get next token, if last on line, ignore */
	        if (next_token(file, buf, sizeof(buf)) != 0)
		    continue;

		/* Got address */
		conf[nconf].adr = inet_addr(buf);
		    
		/* Get next token (mask) */
		i = next_token(file, buf, sizeof(buf));

		/* Only ignore if we got no text at all */
		if (i != EOF)
		{
		    /* Add to list, quit if array is full */
		    conf[nconf++].mask = inet_addr(buf);
		    if (nconf == MAX_LINES) break;
		}

		/* If not at end-of-line, keep reading til we are */
		while (i == 0)
		    i = next_token(file, buf, sizeof(buf));
	    }
	}
	fclose(file);
    }
    else 
    {  (void) sprintf(PQerrormsg,
                      "hba_recvauth: Host-based authentication config file "
                      "does not exist or permissions are not setup correctly! "
                      "Unable to open file \"%s\".\n", 
                      conf_file);
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	free(conf_file);
        return(STATUS_ERROR); 
    }


    /* Config lines now in memory so start checking address */
    /* grab just the address */
    ip_addr = addr->sin_addr.s_addr;

    /*
     * Go through the conf array, turn off the bits given by the mask
     * and then compare the result with the address.  A match means
     * that this address is ok.
     */
    for (i = 0; i < nconf; ++i)
        if ((ip_addr & ~conf[i].mask) == conf[i].adr) return(STATUS_OK);
    
    /* no match, so we can't approve the address */
    return(STATUS_ERROR);
}

/*
 * Grab one token out of fp.  Defined as the next string of non-whitespace
 * in the file.  After we get the token, continue reading until EOF, end of
 * line or the next token.  If it's the last token on the line, return '\n'
 * for the value.  If we get EOF before reading a token, return EOF.  In all
 * other cases return 0.
 */
static int 
next_token(FILE *fp, char *buf, int bufsz)
{
    int c;
    char *eb = buf+(bufsz-1);

    /* Discard inital whitespace */
    while (isspace(c = getc(fp))) ;

    /* EOF seen before any token so return EOF */
    if (c == EOF) return -1;

    /* Form a token in buf */
    do {
	if (buf < eb) *buf++ = c;
	c = getc(fp);
    } while (!isspace(c) && c != EOF);
    *buf = '\0';

    /* Discard trailing tabs and spaces */
    while (c == ' ' || c == '\t') c = getc(fp);

    /* Put back the char that was non-whitespace (putting back EOF is ok) */
    (void) ungetc(c, fp);

    /* If we ended with a newline, return that, otherwise return 0 */
    return (c == '\n' ? '\n' : 0);
}

/*
 * be_recvauth -- server demux routine for incoming authentication information
 */
int
be_recvauth(MsgType msgtype, Port *port, char *username, StartupInfo* sp)
{
    if (!username) {
	(void) sprintf(PQerrormsg,
		       "be_recvauth: no user name passed\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    if (!port) {
	(void) sprintf(PQerrormsg,
		       "be_recvauth: no port structure passed\n");
	fputs(PQerrormsg, stderr);
	pqdebug("%s", PQerrormsg);
	return(STATUS_ERROR);
    }
    
    switch (msgtype) {
#ifdef KRB4
    case STARTUP_KRB4_MSG:
	if (!be_getauthsvc(msgtype)) {
	    (void) sprintf(PQerrormsg,
			   "be_recvauth: krb4 authentication disallowed\n");
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	    return(STATUS_ERROR);
	}
	if (pg_krb4_recvauth(port->sock, &port->laddr, &port->raddr,
			     username) != STATUS_OK) {
	    (void) sprintf(PQerrormsg,
			   "be_recvauth: krb4 authentication failed\n");
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	    return(STATUS_ERROR);
	}
	break;
#endif
#ifdef KRB5
    case STARTUP_KRB5_MSG:
	if (!be_getauthsvc(msgtype)) {
	    (void) sprintf(PQerrormsg,
			   "be_recvauth: krb5 authentication disallowed\n");
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	    return(STATUS_ERROR);
	}
	if (pg_krb5_recvauth(port->sock, &port->laddr, &port->raddr,
			     username) != STATUS_OK) {
	    (void) sprintf(PQerrormsg,
			   "be_recvauth: krb5 authentication failed\n");
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	    return(STATUS_ERROR);
	}
	break;
#endif
    case STARTUP_MSG:
	if (!be_getauthsvc(msgtype)) {
	    (void) sprintf(PQerrormsg,
			   "be_recvauth: unauthenticated connections disallowed failed\n");
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	    return(STATUS_ERROR);
	}
	break;
    case STARTUP_HBA_MSG:
	if (hba_recvauth(&port->raddr, &port->buf, sp) != STATUS_OK) {
	    (void) sprintf(PQerrormsg,
			   "be_recvauth: host-based authentication failed\n");
	    fputs(PQerrormsg, stderr);
	    pqdebug("%s", PQerrormsg);
	    return(STATUS_ERROR);
	}
	break;
    default:
	(void) sprintf(PQerrormsg,
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
	(void) sprintf(PQerrormsg,
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
