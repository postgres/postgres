/*-------------------------------------------------------------------------
 *
 * hba.c--
 *    Routines to handle host based authentication (that's the scheme
 *    wherein you authenticate a user by seeing what IP address the system 
 *    says he comes from and possibly using ident).
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/libpq/hba.c,v 1.15 1997/01/14 01:56:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>    /* needed by in.h on Ultrix */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <postgres.h>
#include <miscadmin.h>
#include <libpq/libpq.h>
#include <libpq/pqcomm.h>
#include <libpq/hba.h>
#include <port/inet_aton.h>    /* For inet_aton() */


#define CONF_FILE "pg_hba.conf"             
  /* Name of the config file  */

#define MAP_FILE "pg_ident.conf"
  /* Name of the usermap file */

#define OLD_CONF_FILE "pg_hba"
  /* Name of the config file in prior releases of Postgres. */

#define MAX_LINES 255                    
  /* Maximum number of config lines that can apply to one database    */

#define MAX_TOKEN 80                    
/* Maximum size of one token in the configuration file  */

#define USERMAP_NAME_SIZE 16  /* Max size of a usermap name */

#define IDENT_PORT 113
  /* Standard TCP port number for Ident service.  Assigned by IANA */

#define IDENT_USERNAME_MAX 512  
  /* Max size of username ident server can return */

enum Userauth {Trust, Ident};

/* Some standard C libraries, including GNU, have an isblank() function.
   Others, including Solaris, do not.  So we have our own.
*/
static bool
isblank(const char c) {
  return(c == ' ' || c == 9 /* tab */);
}



static void 
next_token(FILE *fp, char *buf, const int bufsz) {
/*--------------------------------------------------------------------------
  Grab one token out of fp.  Tokens are strings of non-blank
  characters bounded by blank characters, beginning of line, and end
  of line.  Blank means space or tab.  Return the token as *buf.
  Leave file positioned to character immediately after the token or
  EOF, whichever comes first.  If no more tokens on line, return null
  string as *buf and position file to beginning of next line or EOF,
  whichever comes first.  
--------------------------------------------------------------------------*/
  int c;
  char *eb = buf+(bufsz-1);

  /* Move over inital token-delimiting blanks */
  while (isblank(c = getc(fp))) ;

  if (c != '\n') {
    /* build a token in buf of next characters up to EOF, eol, or blank. */
    while (c != EOF && c != '\n' && !isblank(c)) {
      if (buf < eb) *buf++ = c;
      c = getc(fp);
      /* Put back the char right after the token (putting back EOF is ok) */
    } 
    (void) ungetc(c, fp);
  }
  *buf = '\0';
}



static void
read_through_eol(FILE *file) {
  int c;
  do 
    c = getc(file); 
  while (c != '\n' && c != EOF); 
}



static void
read_hba_entry2(FILE *file, enum Userauth *userauth_p, char usermap_name[], 
                bool *error_p) {
/*--------------------------------------------------------------------------
  Read from file FILE the rest of a host record, after the mask field,
  and return the interpretation of it as *userauth_p, usermap_name, and
  *error_p.
---------------------------------------------------------------------------*/
  char buf[MAX_TOKEN];

  bool userauth_valid;

  /* Get authentication type token. */
  next_token(file, buf, sizeof(buf));
  if (buf[0] == '\0') {
    *error_p = true;
    read_through_eol(file);
  } else {
    if (strcmp(buf, "trust") == 0) {
      userauth_valid = true;
      *userauth_p = Trust;
    } else if (strcmp(buf, "ident") == 0) {
      userauth_valid = true;
      *userauth_p = Ident;
    } else userauth_valid = false;
    if (!userauth_valid) {
      *error_p = true;
      read_through_eol(file);
    } else {
      /* Get the map name token, if any */
      next_token(file, buf, sizeof(buf));
      if (buf[0] == '\0') {
        *error_p = false;
        usermap_name[0] = '\0';
      } else {
        strncpy(usermap_name, buf, USERMAP_NAME_SIZE);
        next_token(file, buf, sizeof(buf));
        if (buf[0] != '\0') {
          *error_p = true;
          read_through_eol(file);
        } else *error_p = false;
      }
    }
  }
}



static void
process_hba_record(FILE *file, 
                   const struct in_addr ip_addr, const char database[],
                   bool *matches_p, bool *error_p, 
                   enum Userauth *userauth_p, char usermap_name[] ) {
/*---------------------------------------------------------------------------
  Process the non-comment record in the config file that is next on the file.
  See if it applies to a connection to a host with IP address "ip_addr"
  to a database named "database[]".  If so, return *matches_p true
  and *userauth_p and usermap_name[] as the values from the entry.
  If not, return matches_p false.  If the record has a syntax error,
  return *error_p true, after issuing a message to stderr.  If no error,
  leave *error_p as it was.
---------------------------------------------------------------------------*/
  char buf[MAX_TOKEN];  /* A token from the record */

  /* Read the record type field */
  next_token(file, buf, sizeof(buf));
  if (buf[0] == '\0') *matches_p = false;
  else {
    /* if this isn't a "host" record, it can't match. */
    if (strcmp(buf, "host") != 0) {
      *matches_p = false;
      read_through_eol(file);
    } else {
      /* It's a "host" record.  Read the database name field. */
      next_token(file, buf, sizeof(buf));
      if (buf[0] == '\0') *matches_p = false;
      else {
        /* If this record isn't for our database, ignore it. */
        if (strcmp(buf, database) != 0 && strcmp(buf, "all") != 0) {
          *matches_p = false;
          read_through_eol(file);
        } else {
          /* Read the IP address field */
          next_token(file, buf, sizeof(buf));
          if (buf[0] == '\0') *matches_p = false;
          else {
            int valid;  /* Field is valid dotted decimal */
            /* Remember the IP address field and go get mask field */
            struct in_addr file_ip_addr;  /* IP address field value */

            valid = inet_aton(buf, &file_ip_addr);
            if (!valid) {
              *matches_p = false;
              read_through_eol(file);
            } else {
              /* Read the mask field */
              next_token(file, buf, sizeof(buf));
              if (buf[0] == '\0') *matches_p = false;
              else {
                struct in_addr mask;
                /* Got mask.  Now see if this record is for our host. */
                valid = inet_aton(buf, &mask);
                if (!valid) {
                  *matches_p = false;
                  read_through_eol(file);
                } else {
                  if (((file_ip_addr.s_addr ^ ip_addr.s_addr) & mask.s_addr)
                      != 0x0000) {
                    *matches_p = false;
                    read_through_eol(file);
                  } else {
                    /* This is the record we're looking for.  Read
                       the rest of the info from it.
                       */
                    read_hba_entry2(file, userauth_p, usermap_name,
                                    error_p);
                    *matches_p = true;
                    if (*error_p) {
                      sprintf(PQerrormsg,
                              "process_hba_record: invalid syntax in "
                              "hba config file "
                              "for host record for IP address %s\n",
                              inet_ntoa(file_ip_addr));
                      fputs(PQerrormsg, stderr);
                      pqdebug("%s", PQerrormsg);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}



static void
process_open_config_file(FILE *file, 
                         const struct in_addr ip_addr, const char database[],
                         bool *host_ok_p, enum Userauth *userauth_p, 
                         char usermap_name[] ) {
/*---------------------------------------------------------------------------
  This function does the same thing as find_hba_entry, only with
  the config file already open on stream descriptor "file".
----------------------------------------------------------------------------*/
  bool found_entry;  
  /* We've processed a record that applies to our connection */
  bool error; 
  /* Said record has invalid syntax. */
  bool eof;  /* We've reached the end of the file we're reading */
  
  found_entry = false;  /* initial value */
  error = false;  /* initial value */
  eof = false;  /* initial value */  
  while (!eof && !found_entry && !error) {
    /* Process a line from the config file */
    
    int c;  /* a character read from the file */
    
    c = getc(file); ungetc(c, file);
    if (c == EOF) eof = true;
    else {
      if (c == '#') read_through_eol(file);
      else {
        process_hba_record(file, ip_addr, database, 
                           &found_entry, &error, userauth_p, usermap_name);
      }
    }    
  }
  if (found_entry) {
    if (error) *host_ok_p = false;
    else *host_ok_p = true;
  } else *host_ok_p = false;
}    



static void
find_hba_entry(const char DataDir[], const struct in_addr ip_addr, 
               const char database[],
               bool *host_ok_p, enum Userauth *userauth_p, 
               char usermap_name[] ) {
/*--------------------------------------------------------------------------
  Read the config file and find an entry that allows connection from
  host "ip_addr" to database "database".  If not found, return 
  *host_ok_p == false.  If found, return *userauth_p and *usermap_name
  representing the contents of that entry.

  When a record has invalid syntax, we either ignore it or reject the
  connection (depending on where it's invalid).  No message or anything.
  We need to fix that some day.

  If we don't find or can't access the config file, we issue an error
  message and deny the connection.

  If we find a file by the old name of the config file (pg_hba), we issue
  an error message because it probably needs to be converted.  He didn't
  follow directions and just installed his old hba file in the new database
  system.

---------------------------------------------------------------------------*/ 
  int rc;
  struct stat statbuf;

  FILE *file;  /* The config file we have to read */

  char *old_conf_file;  
    /* The name of old config file that better not exist. */

  /* Fail if config file by old name exists. */


  /* put together the full pathname to the old config file */
  old_conf_file = (char *) malloc((strlen(DataDir) +
                                   strlen(OLD_CONF_FILE)+2)*sizeof(char));
  sprintf(old_conf_file, "%s/%s", DataDir, OLD_CONF_FILE);
  
  rc = stat(old_conf_file, &statbuf);
  if (rc == 0) {  
    /* Old config file exists.  Tell this guy he needs to upgrade. */
    sprintf(PQerrormsg,
            "A file exists by the name used for host-based authentication "
            "in prior releases of Postgres (%s).  The name and format of "
            "the configuration file have changed, so this file should be "
            "converted.\n",
            old_conf_file);
    fputs(PQerrormsg, stderr);
    pqdebug("%s", PQerrormsg);
  } else {
    char *conf_file;  /* The name of the config file we have to read */

    /* put together the full pathname to the config file */
    conf_file = (char *) malloc((strlen(DataDir) +
                                 strlen(CONF_FILE)+2)*sizeof(char));
    sprintf(conf_file, "%s/%s", DataDir, CONF_FILE);
    
    file = fopen(conf_file, "r");
    if (file == 0) {  
      /* The open of the config file failed.  */
      
      *host_ok_p = false;

      sprintf(PQerrormsg,
              "find_hba_entry: Host-based authentication config file "
              "does not exist or permissions are not setup correctly! "
              "Unable to open file \"%s\".\n", 
              conf_file);
      fputs(PQerrormsg, stderr);
      pqdebug("%s", PQerrormsg);
    } else {
      process_open_config_file(file, ip_addr, database, host_ok_p, userauth_p,
                               usermap_name);
      fclose(file);
    }
    free(conf_file);
  }
  free(old_conf_file);
  return;
}


static void
interpret_ident_response(char ident_response[], 
                         bool *error_p, char ident_username[]) {
/*----------------------------------------------------------------------------
  Parse the string "ident_response[]" as a response from a query to an Ident
  server.  If it's a normal response indicating a username, return 
  *error_p == false and the username as ident_username[].  If it's anything
  else, return *error_p == true and ident_username[] undefined.
----------------------------------------------------------------------------*/
  char *cursor;  /* Cursor into ident_response[] */
  
  cursor = &ident_response[0];

  /* Ident's response, in the telnet tradition, should end in crlf (\r\n). */
  if (strlen(ident_response) < 2) *error_p = true;
  else if (ident_response[strlen(ident_response)-2] != '\r') *error_p = true;
  else {
    while (*cursor != ':' && *cursor != '\r') cursor++;  /* skip port field */
  
    if (*cursor != ':') *error_p = true;
    else {
      /* We're positioned to colon before response type field */
      char response_type[80];
      int i;   /* Index into response_type[] */
      cursor++;  /* Go over colon */
      while (isblank(*cursor)) cursor++;  /* skip blanks */
      i = 0;
      while (*cursor != ':' && *cursor != '\r' && !isblank(*cursor) 
             && i < sizeof(response_type)-1) 
        response_type[i++] = *cursor++;
      response_type[i] = '\0';
      while (isblank(*cursor)) cursor++;  /* skip blanks */
      if (strcmp(response_type, "USERID") != 0)
        *error_p = true;
      else {
        /* It's a USERID response.  Good.  "cursor" should be pointing to
           the colon that precedes the operating system type.
           */
        if (*cursor != ':') *error_p = true;
        else {
          cursor++;  /* Go over colon */
          /* Skip over operating system field. */
          while (*cursor != ':' && *cursor != '\r') cursor++;
          if (*cursor != ':') *error_p = true;
          else {
            int i;  /* Index into ident_username[] */
            cursor ++;   /* Go over colon */
            while (isblank(*cursor)) cursor++;   /* skip blanks */
            /* Rest of line is username.  Copy it over. */
            i = 0;
            while (*cursor != '\r' && i < IDENT_USERNAME_MAX) 
              ident_username[i++] = *cursor++;
            ident_username[i] = '\0';
            *error_p = false;
          }
        }
      }
    }
  }
}



static void
ident(const struct in_addr remote_ip_addr, const struct in_addr local_ip_addr,
      const ushort remote_port, const ushort local_port,
      bool *ident_failed, char ident_username[]) {
/*--------------------------------------------------------------------------
  Talk to the ident server on host "remote_ip_addr" and find out who
  owns the tcp connection from his port "remote_port" to port
  "local_port_addr" on host "local_ip_addr".  Return the username the
  ident server gives as "ident_username[]".

  IP addresses and port numbers are in network byte order.

  But iff we're unable to get the information from ident, return
  *ident_failed == true (and ident_username[] undefined).
----------------------------------------------------------------------------*/

  int sock_fd;
    /* File descriptor for socket on which we talk to Ident */

  int rc;  /* Return code from a locally called function */

  sock_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock_fd == -1) {
    sprintf(PQerrormsg,
            "Failed to create socket on which to talk to Ident server. "
            "socket() returned errno = %s (%d)\n",
            strerror(errno), errno);
    fputs(PQerrormsg, stderr);
    pqdebug("%s", PQerrormsg);
  } else {
    struct sockaddr_in ident_server;
      /* Socket address of Ident server on the system from which client 
         is attempting to connect to us.
         */
    ident_server.sin_family = AF_INET;
    ident_server.sin_port = htons(IDENT_PORT);
    ident_server.sin_addr = remote_ip_addr;
    rc = connect(sock_fd, 
                 (struct sockaddr *) &ident_server, sizeof(ident_server));
    if (rc != 0) {
      sprintf(PQerrormsg,
              "Unable to connect to Ident server on the host which is "
              "trying to connect to Postgres "
              "(IP address %s, Port %d). "
              "errno = %s (%d)\n",
              inet_ntoa(remote_ip_addr), IDENT_PORT, strerror(errno), errno);
      fputs(PQerrormsg, stderr);
      pqdebug("%s", PQerrormsg);
      *ident_failed = true;
    } else {
      char ident_query[80];
        /* The query we send to the Ident server */
      sprintf(ident_query, "%d,%d\n", 
              ntohs(remote_port), ntohs(local_port));
      rc = send(sock_fd, ident_query, strlen(ident_query), 0);
      if (rc < 0) {
        sprintf(PQerrormsg,
                "Unable to send query to Ident server on the host which is "
                "trying to connect to Postgres (Host %s, Port %d),"
                "even though we successfully connected to it.  "
                "errno = %s (%d)\n",
                inet_ntoa(remote_ip_addr), IDENT_PORT, strerror(errno), errno);
        fputs(PQerrormsg, stderr);
        pqdebug("%s", PQerrormsg);
        *ident_failed = true;
      } else {
        char ident_response[80+IDENT_USERNAME_MAX];
        rc = recv(sock_fd, ident_response, sizeof(ident_response)-1, 0);
        if (rc < 0) {
          sprintf(PQerrormsg,
                  "Unable to receive response from Ident server "
                  "on the host which is "
                  "trying to connect to Postgres (Host %s, Port %d),"
                  "even though we successfully sent our query to it.  "
                  "errno = %s (%d)\n",
                  inet_ntoa(remote_ip_addr), IDENT_PORT, 
                  strerror(errno), errno);
          fputs(PQerrormsg, stderr);
          pqdebug("%s", PQerrormsg);
          *ident_failed = true;
        } else {
          bool error;  /* response from Ident is garbage. */
          ident_response[rc] = '\0';
          interpret_ident_response(ident_response, &error, ident_username);
          *ident_failed = error;
        }
      }
      close(sock_fd);
    }
  }
}



static void
parse_map_record(FILE *file, 
                 char file_map[], char file_pguser[], char file_iuser[]) {
/*---------------------------------------------------------------------------
  Take the noncomment line which is next on file "file" and interpret
  it as a line in a usermap file.  Specifically, return the first
  3 tokens as file_map, file_iuser, and file_pguser, respectively.  If
  there are fewer than 3 tokens, return null strings for the missing
  ones.

---------------------------------------------------------------------------*/
  char buf[MAX_TOKEN];
    /* A token read from the file */

  /* Set defaults in case fields not in file */
  file_map[0] = '\0';
  file_pguser[0] = '\0';
  file_iuser[0] = '\0';

  next_token(file, buf, sizeof(buf));
  if (buf != '\0') {
    strcpy(file_map, buf);
    next_token(file, buf, sizeof(buf));
    if (buf != '\0') {
      strcpy(file_iuser, buf);
      next_token(file, buf, sizeof(buf));
      if (buf != '\0') {
        strcpy(file_pguser, buf);
        read_through_eol(file);
      }
    }
  }
}



static void
verify_against_open_usermap(FILE *file,
                            const char pguser[], 
                            const char ident_username[], 
                            const char usermap_name[],
                            bool *checks_out_p) {
/*--------------------------------------------------------------------------
  This function does the same thing as verify_against_usermap,
  only with the config file already open on stream descriptor "file".
---------------------------------------------------------------------------*/
  bool match;  /* We found a matching entry in the map file */
  bool eof;  /* We've reached the end of the file we're reading */
  
  match = false;  /* initial value */
  eof = false;  /* initial value */  
  while (!eof && !match) {
    /* Process a line from the map file */
    
    int c;  /* a character read from the file */
    
    c = getc(file); ungetc(c, file);
    if (c == EOF) eof = true;
    else {
      if (c == '#') read_through_eol(file);
      else {
        /* The following are fields read from a record of the file */
        char file_map[MAX_TOKEN+1];
        char file_pguser[MAX_TOKEN+1];
        char file_iuser[MAX_TOKEN+1];

        parse_map_record(file, file_map, file_pguser, file_iuser);
        if (strcmp(file_map, usermap_name) == 0 &&
            strcmp(file_pguser, pguser) == 0 &&
            strcmp(file_iuser, ident_username) == 0)
          match = true;
      }
    }    
  }
  *checks_out_p = match;
}



static void
verify_against_usermap(const char DataDir[],
                       const char pguser[], 
                       const char ident_username[], 
                       const char usermap_name[],
                       bool *checks_out_p) {
/*--------------------------------------------------------------------------
  See if the user with ident username "ident_username" is allowed to act
  as Postgres user "pguser" according to usermap "usermap_name".   Look
  it up in the usermap file.

  Special case: For usermap "sameuser", don't look in the usermap
  file.  That's an implied map where "pguser" must be identical to
  "ident_username" in order to be authorized.

  Iff authorized, return *checks_out_p == true.
  
--------------------------------------------------------------------------*/

  if (usermap_name[0] == '\0') {
    *checks_out_p = false;
    sprintf(PQerrormsg,
            "verify_against_usermap: hba configuration file does not "
            "have the usermap field filled in in the entry that pertains "
            "to this connection.  That field is essential for Ident-based "
            "authentication.\n");
      fputs(PQerrormsg, stderr);
      pqdebug("%s", PQerrormsg);
  } else if (strcmp(usermap_name, "sameuser") == 0) {
    if (strcmp(ident_username, pguser) == 0) *checks_out_p = true;
    else *checks_out_p = false;
  } else {
    FILE *file;  /* The map file we have to read */

    char *map_file;  /* The name of the map file we have to read */

    /* put together the full pathname to the map file */
    map_file = (char *) malloc((strlen(DataDir) +
                                strlen(MAP_FILE)+2)*sizeof(char));
    sprintf(map_file, "%s/%s", DataDir, MAP_FILE);
    
    file = fopen(map_file, "r");
    if (file == 0) {  
      /* The open of the map file failed.  */
      
      *checks_out_p = false;

      sprintf(PQerrormsg,
              "verify_against_usermap: usermap file for Ident-based "
              "authentication "
              "does not exist or permissions are not setup correctly! "
              "Unable to open file \"%s\".\n", 
              map_file);
      fputs(PQerrormsg, stderr);
      pqdebug("%s", PQerrormsg);
    } else {
      verify_against_open_usermap(file, 
                                  pguser, ident_username, usermap_name,
                                  checks_out_p);
      fclose(file);
    }
    free(map_file);


  }
}



static void
authident(const char DataDir[], 
          const Port port, const char postgres_username[], 
          const char usermap_name[],
          bool *authentic_p) {
/*---------------------------------------------------------------------------
  Talk to the ident server on the remote host and find out who owns the 
  connection described by "port".  Then look in the usermap file under
  the usermap usermap_name[] and see if that user is equivalent to 
  Postgres user user[].

  Return *authentic_p true iff yes.
---------------------------------------------------------------------------*/
  bool ident_failed;
    /* We were unable to get ident to give us a username */
  char ident_username[IDENT_USERNAME_MAX+1];
    /* The username returned by ident */

  ident(port.raddr.sin_addr, port.laddr.sin_addr, 
        port.raddr.sin_port, port.laddr.sin_port,
        &ident_failed, ident_username);

  if (ident_failed) *authentic_p = false;
  else {
    bool checks_out;
    verify_against_usermap(DataDir, 
                           postgres_username, ident_username, usermap_name,
                           &checks_out);
    if (checks_out) *authentic_p = true;
    else *authentic_p = false;
  }
}



extern int
hba_recvauth(const Port *port, const char database[], const char user[],
             const char DataDir[]) {
/*---------------------------------------------------------------------------
  Determine if the TCP connection described by "port" is with someone
  allowed to act as user "user" and access database "database".  Return
  STATUS_OK if yes; STATUS_ERROR if not.
----------------------------------------------------------------------------*/ 
  bool host_ok;  
    /* There's an entry for this database and remote host in the pg_hba file */
  char usermap_name[USERMAP_NAME_SIZE+1];
    /* The name of the map pg_hba specifies for this connection (or special
       value "SAMEUSER")
    */
  enum Userauth userauth;
    /* The type of user authentication pg_hba specifies for this connection */
  int retvalue;
    /* Our eventual return value */


  find_hba_entry(DataDir, port->raddr.sin_addr, database, 
                 &host_ok, &userauth, usermap_name);
  
  if (!host_ok) retvalue = STATUS_ERROR;
  else {
    switch (userauth) {
    case Trust: 
      retvalue = STATUS_OK;
      break;
    case Ident: {
      /* Here's where we need to call up ident and authenticate the user */
    
      bool authentic;  /* He is who he says he is. */

      authident(DataDir, *port, user, usermap_name, &authentic);

      if (authentic) retvalue = STATUS_OK;
      else retvalue = STATUS_ERROR;
    }
      break;
    default:
      retvalue = STATUS_ERROR;
      Assert(false);
    }
  }
  return(retvalue);
}


/*----------------------------------------------------------------
 * This version of hba was written by Bryan Henderson
 * in September 1996 for Release 6.0.  It changed the format of the 
 * hba file and added ident function.
 *
 * Here are some notes about the original host based authentication
 * the preceded this one.  
 *
 * based on the securelib package originally written by William
 * LeFebvre, EECS Department, Northwestern University
 * (phil@eecs.nwu.edu) - orginal configuration file code handling
 * by Sam Horrocks (sam@ics.uci.edu)
 *
 * modified and adapted for use with Postgres95 by Paul Fisher
 * (pnfisher@unity.ncsu.edu)
 *
 -----------------------------------------------------------------*/

