/*-------------------------------------------------------------------------
 *
 * monitor.c--
 *    POSTGRES Terminal Monitor
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/monitor/Attic/monitor.c,v 1.4 1996/07/23 02:26:41 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <errno.h>
#include "libpq/pqsignal.h"	/* substitute for <signal.h> */
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/param.h>	/* for MAXHOSTNAMELEN on most */
#ifndef WIN32
#include <unistd.h>
#endif
#ifdef PORTNAME_sparc_solaris
#include <netdb.h>	/* for MAXHOSTNAMELEN on some */
#endif
#include <sys/types.h>
/* #include <sys/uio.h> */
#include <time.h>

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

extern char	*getenv();

/* 
 * monitor.c  -- function prototypes (all private)
 */
static void do_input(FILE *ifp);
static void init_tmon();
static void welcome();
static void handle_editor();
static void handle_shell();
static void handle_send();
static int handle_execution(char *query); 
static void handle_file_insert(FILE *ifp);
static void handle_print();
static void handle_exit(int exit_status);
static void handle_clear();
static void handle_print_time();
static int handle_write_to_file();
static void handle_help();
static void stuff_buffer(char c);
static void argsetup(int *argcP, char ***argvP);
static void handle_copy_out(PGresult *res);
static void handle_copy_in(PGresult *res);


/*      
 *          Functions which maintain the logical query buffer in
 *          /tmp/PQxxxxx.  It in general just does a copy from input
 *          to query buffer, unless it gets a backslash escape character.
 *          It recognizes the following escapes:
 *      
 *          \e -- enter editor
 *          \g -- "GO": submit query to POSTGRES
 *          \i -- include (switch input to external file)
 *          \p -- print query buffer 
 *          \q -- quit POSTGRES
 *          \r -- force reset (clear) of query buffer
 *          \s -- call shell
 *          \t -- print current time
 *          \w -- write query buffer to external file
 *          \h -- print the list of commands
 *          \? -- print the list of commands
 *          \\ -- produce a single backslash in query buffer
 *      
 */

/*
 *   Declaration of global variables (but only to the file monitor.c
 */

#define DEFAULT_EDITOR "/usr/ucb/vi"
#define COPYBUFSIZ	8192
static char *user_editor;     /* user's desired editor  */
static int tmon_temp;         /* file descriptor for temp. buffer file */
static char *tmon_temp_filename;
static char query_buffer[8192];  /* Max postgres buffer size */
static char *RunOneFile = NULL;
bool RunOneCommand = false;
bool Debugging;
bool Verbose;
bool Silent;
bool TerseOutput = false;
bool PrintAttNames = true;
bool SingleStepMode = false;
bool SemicolonIsGo = true;

#define COLWIDTH 12

extern char *optarg;
extern int optind,opterr;
FILE *debug_port;

/*
 *  As of release 4, we allow the user to specify options in the environment
 *  variable PGOPTION.  These are treated as command-line options to the
 *  terminal monitor, and are parsed before the actual command-line args.
 *  The arge struct is used to construct an argv we can pass to getopt()
 *  containing the union of the environment and command line arguments.
 */

typedef struct arge {
    char	*a_arg;
    struct arge	*a_next;
} arge;

/* the connection to the backend */
PGconn *conn;

void
main(int argc, char **argv)
{
    int c;
    int errflag = 0;
    char *progname;
    char *debug_file;
    char *dbname;
    char *command;
    int exit_status = 0;
    char errbuf[ERROR_MSG_LENGTH];
    char *username, usernamebuf[NAMEDATALEN + 1];

    char *pghost = NULL;
    char *pgtty = NULL;
    char *pgoptions = NULL;
    char *pgport = NULL;
    int  pgtracep = 0;

    /* 
     * Processing command line arguments.
     *
     * h : sets the hostname.
     * p : sets the coom. port
     * t : sets the tty.
     * o : sets the other options. (see doc/libpq)
     * d : enable debugging mode.
     * q : run in quiet mode
     * Q : run in VERY quiet mode (no output except on errors)
     * c : monitor will run one POSTQUEL command and exit
     *
     * s : step mode (pauses after each command)
     * S : don't use semi colon as \g
     *
     * T : terse mode - no formatting
     * N : no attribute names - only columns of data
     *     (these two options are useful in conjunction with the "-c" option
     *      in scripts.)
     */

    progname = *argv;
    Debugging = false;
    Verbose = true;
    Silent = false;

    /* prepend PGOPTION, if any */
    argsetup(&argc, &argv);

    while ((c = getopt(argc, argv, "a:h:f:p:t:d:qsSTNQc:")) != EOF) {
	switch (c) {
	    case 'a':
	      fe_setauthsvc(optarg, errbuf);
	      break;
	    case 'h' :
	      pghost = optarg;
	      break;
	    case 'f' :
	      RunOneFile = optarg;
	      break;
	    case 'p' :
	      pgport = optarg;
	      break;
	    case 't' :
	      pgtty = optarg;
	      break;
	    case 'T' :
	      TerseOutput = true;
	      break;
	    case 'N' :
	      PrintAttNames = false;
	      break;
	    case 'd' :

	      /*
	       *  When debugging is turned on, the debugging messages
	       *  will be sent to the specified debug file, which
	       *  can be a tty ..
	       */

	      Debugging = true;
	      debug_file = optarg;
	      debug_port = fopen(debug_file,"w+");
	      if (debug_port == NULL) {
		  fprintf(stderr,"Unable to open debug file %s \n", debug_file);
		  exit(1);
	      }
	      pgtracep = 1;
	      break;
	    case 'q' :
	      Verbose = false;
	      break;
	    case 's' :
	      SingleStepMode = true;
	      SemicolonIsGo = true;
	      break;
	    case 'S' :
	      SemicolonIsGo = false;
	      break;
	    case 'Q' :
	      Verbose = false;
	      Silent = true;
	      break;
	    case 'c' :
	      Verbose = false;
	      Silent = true;
	      RunOneCommand = true;
	      command = optarg;
	      break;
	    case '?' :
	    default :
	      errflag++;
	      break;
	}
    }

    if (errflag ) {
      fprintf(stderr, "usage: %s [options...] [dbname]\n", progname);
      fprintf(stderr, "\t-a authsvc\tset authentication service\n");
      fprintf(stderr, "\t-c command\t\texecute one command\n");
      fprintf(stderr, "\t-d debugfile\t\tdebugging output file\n");
      fprintf(stderr, "\t-h host\t\t\tserver host name\n");
      fprintf(stderr, "\t-f file\t\t\trun query from file\n");
      fprintf(stderr, "\t-p port\t\t\tserver port number\n");
      fprintf(stderr, "\t-q\t\t\tquiet output\n");
      fprintf(stderr, "\t-t logfile\t\terror-logging tty\n");
      fprintf(stderr, "\t-N\t\t\toutput without attribute names\n");
      fprintf(stderr, "\t-Q\t\t\tREALLY quiet output\n");
      fprintf(stderr, "\t-T\t\t\tterse output\n");
      exit(2);
    }

    /* Determine our username (according to the authentication system, if
     * there is one).
     */
    if ((username = fe_getauthname(errbuf)) == (char *) NULL) {
	    fprintf(stderr, "%s: could not find a valid user name\n",
		    progname);
	    exit(2);
    }
    memset(usernamebuf, 0, sizeof(usernamebuf));
    (void) strncpy(usernamebuf, username, NAMEDATALEN);
    username = usernamebuf;
    
    /* find database */
    if (!(dbname = argv[optind]) &&
	!(dbname = getenv("DATABASE")) &&
	!(dbname = username)) {
	    fprintf(stderr, "%s: no database name specified\n", progname);
	    exit (2);
    }

    conn = PQsetdb(pghost, pgport, pgoptions, pgtty, dbname);
    if (PQstatus(conn) == CONNECTION_BAD) {
      fprintf(stderr,"Connection to database '%s' failed.\n", dbname);
      fprintf(stderr,"%s",PQerrorMessage(conn));
      exit(1);
    }

    if (pgtracep)
      PQtrace(conn,debug_port);

    /* print out welcome message and start up */
    welcome();
    init_tmon(); 

    /* parse input */
    if (RunOneCommand) {
	exit_status = handle_execution(command);
    } else if (RunOneFile) {
	bool oldVerbose;
	FILE *ifp;

	if ((ifp = fopen(RunOneFile, "r")) == NULL) {
	    fprintf(stderr, "Cannot open %s\n", RunOneFile);
	}
	
	if (SingleStepMode) {
	    oldVerbose = Verbose;
	    Verbose = false;
	}
	do_input(ifp);
	fclose(ifp);
	if (SingleStepMode)
	    Verbose = oldVerbose;
    } else {
	do_input(stdin);
    }

    handle_exit(exit_status);
}

static void
do_input(FILE *ifp)
{
    int c;
    char escape;

    /*
     *  Processing user input.
     *  Basically we stuff the user input to a temp. file until
     *  an escape char. is detected, after which we switch
     *  to the appropriate routine to handle the escape.
     */

    if (ifp == stdin) {
	if (Verbose)
	    fprintf(stdout,"\nGo \n* ");
	else {
	    if (!Silent)
		fprintf(stdout, "* ");
	}
    }
    while ((c = getc(ifp)) != EOF ) {
	if ( c == '\\') {
	    /* handle escapes */
	    escape = getc(ifp);
	    switch( escape ) {
	      case 'e':
		handle_editor();
		break;
	      case 'g':
		handle_send();
		break;
	      case 'i':
		{
		    bool oldVerbose;

		    if (SingleStepMode) {
			oldVerbose = Verbose;
			Verbose = false;
		    }
		    handle_file_insert(ifp);
		    if (SingleStepMode)
			Verbose = oldVerbose;
		}
		break;
	      case 'p':
		handle_print();
		break;
	      case 'q':
		handle_exit(0);
		break;
	      case 'r':
		handle_clear();
		break;
	      case 's':
		handle_shell();
		break;
	      case 't':
		handle_print_time();
		break;
	      case 'w':
		handle_write_to_file();
		break;
	      case '?':
	      case 'h':
		handle_help();
		break;
	      case '\\':
		c = escape;
		stuff_buffer(c); 
		break;
	      case ';':
		c = escape;
		stuff_buffer(c);
		break;
	      default:
		fprintf(stderr, "unknown escape given\n");
		break;
	    } /* end-of-switch */
	    if (ifp == stdin && escape != '\\') {
		if (Verbose)
		    fprintf(stdout,"\nGo \n* ");
		else {
		    if (!Silent)
			fprintf(stdout, "* ");
		}
	    }
	} else {
	    stuff_buffer(c);
	    if (c == ';' && SemicolonIsGo) {
		handle_send();
		if (Verbose)
		    fprintf(stdout,"\nGo \n* ");
		else {
		    if (!Silent)
			fprintf(stdout, "* ");
		}
	    }
	}
    }
}

/*
 * init_tmon()
 *
 * set the following :
 *     user_editor, defaults to DEFAULT_EDITOR if env var is not set
 */
static void
init_tmon()
{
    if (!RunOneCommand)
    {
	char *temp_editor = getenv("EDITOR");
    
	if (temp_editor != NULL) 
	    user_editor = temp_editor;
	else
	    user_editor = DEFAULT_EDITOR;

	tmon_temp_filename = malloc(20);
	sprintf(tmon_temp_filename, "/tmp/PQ%d", getpid());
	tmon_temp = open(tmon_temp_filename,O_CREAT | O_RDWR | O_APPEND,0666);
    }

    /*
     * Catch signals so we can delete the scratch file GK
     * but only if we aren't already ignoring them -mer
     */

#ifndef WIN32
    if (signal(SIGHUP, SIG_IGN) != SIG_IGN)
	signal(SIGHUP, handle_exit);
    if (signal(SIGQUIT, SIG_IGN) != SIG_IGN)
	signal(SIGQUIT, handle_exit);
#endif /* WIN32 we'll have to figure out how to handle these */
    if (signal(SIGTERM, SIG_IGN) != SIG_IGN)
	signal(SIGTERM, handle_exit);
    if (signal(SIGINT, SIG_IGN) != SIG_IGN)
	signal(SIGINT, handle_exit);
}

/*
 *  welcome simply prints the Postgres welcome mesg.
 */
static
void welcome()
{
    if (Verbose) {
	fprintf(stdout,"Welcome to the POSTGRES95 terminal monitor\n");
	fprintf(stdout,"  Please read the file COPYRIGHT for copyright terms of POSTGRES95\n");
    }
}


/*
 *  handle_editor()
 *
 *  puts the user into edit mode using the editor specified
 *  by the variable "user_editor".
 */
static void
handle_editor()
{
    char edit_line[100];

    close(tmon_temp);
    sprintf(edit_line,"%s %s",user_editor,tmon_temp_filename);
    system(edit_line);
    tmon_temp = open(tmon_temp_filename,O_CREAT | O_RDWR | O_APPEND,0666);
}

static void
handle_shell()
{
    char *user_shell;

    user_shell = getenv("SHELL");
    if (user_shell != NULL) {
        system(user_shell);
    } else {
        system("/bin/sh");
    }
}

/*
 * handle_send()
 *
 * This is the routine that initialises the comm. with the
 * backend.  After the tuples have been returned and 
 * displayed, the query_buffer is cleared for the 
 * next query.
 *
 */

#include <ctype.h>

static void
handle_send()
{
    char c  = (char)0;
    off_t pos;
    int cc = 0;
    int i = 0;

    pos = lseek(tmon_temp, (off_t) 0, SEEK_SET);

    if (pos != 0)
	fprintf(stderr, "Bogus file position\n");

    if (Verbose)
	printf("\n");

    /* discard leading white space */
    while ( ( cc = read(tmon_temp,&c,1) ) != 0 && isspace((int)c))
	continue;

    if ( cc != 0 ) {
	pos = lseek(tmon_temp, (off_t) -1, SEEK_CUR);
    }

    if (SingleStepMode) {
	char buf[1024];
	fprintf(stdout, "\n*******************************************************************************\n");
	while ((cc = read(tmon_temp,buf,1024))>0) {
	    buf[cc] = '\0';
	    fprintf(stdout, "%s", buf);
	}
	fprintf(stdout, "\n*******************************************************************************\n\n");
	(void)lseek(tmon_temp, (off_t)pos, SEEK_SET);
    }

    query_buffer[0] = 0;

    /*
     *  Stripping out comments (if any) from the query (should really be
     *  handled in the parser, of course).
     */
    while ( ( cc = read(tmon_temp,&c,1) ) != 0) {
	switch(c) {
	case '\n':
	    query_buffer[i++] = ' ';
	    break;
	case '-': {
	    int temp; 
	    char temp_c;
	    if ((temp = read(tmon_temp,&temp_c,1)) > 0) {
		if (temp_c == '-' ) {
		    /* read till end of line */
		    while ((temp = read(tmon_temp,&temp_c,1)) != 0) {
			if (temp_c=='\n')
			    break;
		    }
		}else {
		    query_buffer[i++] = c;
		    query_buffer[i++] = temp_c;
		}
	    } else {
		query_buffer[i++] = c;
	    }
	    break;
	}
	case '$': {
	    int temp;
	    char temp_c[4];
	    /*
	     * monitor feature, not POSTGRES SQL. When monitor sees $PWD,
	     * it will substitute in the current directory.
	     */
	    if ((temp = read(tmon_temp,temp_c,3)) > 0) {
		temp_c[temp] = '\0';
		if (!strncmp(temp_c, "PWD", 3)) {
		    int len;
		    char cwdPath[MAXPATHLEN];
		    if (getcwd(cwdPath, MAXPATHLEN)==NULL) {
			fprintf(stderr,
				"cannot get current working directory\n");
			break;
		    }
		    len = strlen(cwdPath);
		    query_buffer[i] = '\0';
		    strcat(query_buffer, cwdPath);
		    i += len;
		} else {
		    int j;
		    query_buffer[i++] = c;
		    for(j = 0; j < temp; j++) {
			query_buffer[i++] = temp_c[j];
		    }
		}
	    } else {
		query_buffer[i++] = c;
	    }
	    break;
	}
	default:
	    query_buffer[i++] = c;
	    break;
	}
    }

    if (query_buffer[0] == 0) {
        query_buffer[0] = ' ';
        query_buffer[1] = 0;
    }

    if (Verbose && !SingleStepMode)
	fprintf(stdout,"Query sent to backend is \"%s\"\n", query_buffer);

    fflush(stderr);
    fflush(stdout);
    
    /*
     * Repeat commands until done.
     */

	handle_execution(query_buffer);

    /* clear the query buffer and temp file -- this is very expensive */
    handle_clear();
    memset(query_buffer,0,i);
}

/*
 * Actually execute the query in *query.
 *
 * Returns 0 if the query finished successfully, 1 otherwise.
 */
static int
handle_execution(char *query) 
{
    PGresult *result;
    int retval = 0;
    PQprintOpt opt;
    
    result = PQexec(conn, query);

    if (result == NULL) {
      fprintf(stderr,"%s", PQerrorMessage(conn));
      return 1;
    }

    switch (PQresultStatus(result)) {
    case PGRES_EMPTY_QUERY:
	break;
    case PGRES_COMMAND_OK:
	break;
    case PGRES_TUPLES_OK:
/*	PQprintTuples(result,stdout,PrintAttNames,TerseOutput,COLWIDTH); */
/* 	if (TerseOutput)
 	    PQdisplayTuples(result,stdout,1,"",PrintAttNames,TerseOutput);
 	else
 	    PQdisplayTuples(result,stdout,1,"|",PrintAttNames,TerseOutput); */
        memset(&opt, 0, sizeof opt);
        opt.header = opt.align = opt.standard = 1;
	if (TerseOutput)
	    opt.fieldSep = "";
	else
	    opt.fieldSep = "|";
        PQprint(stdout, result, &opt);
        break;
    case PGRES_COPY_OUT:
	handle_copy_out(result);
	break;
    case PGRES_COPY_IN:
	handle_copy_in(result);
	break;
    case PGRES_BAD_RESPONSE:
	retval = 1;
	break;
    case PGRES_NONFATAL_ERROR:
	retval = 1;
	break;
    case PGRES_FATAL_ERROR:
	retval = 1;
	break;
    }

    if (SingleStepMode) {
	fflush(stdin);
	printf("\npress return to continue ...\n");
	getc(stdin);	/* assume stdin is not a file! */
    }
    return(retval);
}

/*
 * handle_file_insert()
 *
 * allows the user to insert a query file and execute it.
 * NOTE: right now the full path name must be specified.
 */
static void
handle_file_insert(FILE *ifp)
{
    char user_filename[50];
    FILE *nifp;

    fscanf(ifp, "%s",user_filename);
    nifp = fopen(user_filename, "r");
    if (nifp == (FILE *) NULL) {
        fprintf(stderr, "Cannot open %s\n", user_filename);
    } else {
        do_input(nifp);
        fclose (nifp);
    }
}

/*
 * handle_print()
 *
 * This routine prints out the contents (query) of the temp. file
 * onto stdout.
 */
static void
handle_print()
{
    char c;
    off_t pos;
    int cc;
    
    pos = lseek(tmon_temp, (off_t) 0, SEEK_SET);
    
    if (pos != 0 )
	fprintf(stderr, "Bogus file position\n");
    
    printf("\n");
    
    while ( ( cc = read(tmon_temp,&c,1) ) != 0) 
	putchar(c);
    
    printf("\n");
}


/*
 * handle_exit()
 *
 * ends the comm. with the backend and exit the tm.
 */
static void
handle_exit(int exit_status)
{
    if (!RunOneCommand) {
	close(tmon_temp);
	unlink(tmon_temp_filename);
    }
    PQfinish(conn);   
    exit(exit_status);
}

/*
 * handle_clear()
 *
 *  This routine clears the temp. file.
 */
static void
handle_clear()
{
    /* high cost */
    close(tmon_temp);
    tmon_temp = open(tmon_temp_filename,O_TRUNC|O_RDWR|O_CREAT ,0666);
}

/*
 * handle_print_time()
 * prints out the date using the "date" command.
 */
static void
handle_print_time()
{
    system("date");
}

/*
 * handle_write_to_file()
 *
 * writes the contents of the temp. file to the
 * specified file.
 */
static int
handle_write_to_file()
{
    char filename[50];
    static char command_line[512];
    int status;
    
    status = scanf("%s", filename);
    if (status < 1 || !filename[0]) {
	fprintf(stderr, "error: filename is empty\n");
	return(-1);
    }
    
    /* XXX portable way to check return status?  $%&! ultrix ... */
    (void) sprintf(command_line, "rm -f %s", filename);
    (void) system(command_line);
    (void) sprintf(command_line, "cp %s %s", tmon_temp_filename, filename);
    (void) system(command_line);

    return(0);
}

/*
 *
 * Prints out a help message.
 *
 */
static void
handle_help()
{
    printf("Available commands include \n\n");
    printf("\\e -- enter editor\n");
    printf("\\g -- \"GO\": submit query to POSTGRES\n");
    printf("\\i -- include (switch input to external file)\n");
    printf("\\p -- print query buffer\n");
    printf("\\q -- quit POSTGRES\n");
    printf("\\r -- force reset (clear) of query buffer\n");
    printf("\\s -- shell escape \n");
    printf("\\t -- print current time\n");
    printf("\\w -- write query buffer to external file\n");
    printf("\\h -- print the list of commands\n");
    printf("\\? -- print the list of commands\n");
    printf("\\\\ -- produce a single backslash in query buffer\n");
    fflush(stdin);
}

/*
 * stuff_buffer()
 *
 * writes the user input into the temp. file.
 */
static void
stuff_buffer(char c)
{
    int cc;

    cc = write(tmon_temp,&c,1);

    if(cc == -1)
	fprintf(stderr, "error writing to temp file\n");
}

static void
argsetup(int *argcP, char ***argvP)
{
    int argc;
    char **argv, **curarg;
    char *eopts;
    char *envopts;
    int neopts;
    char *start, *end;
    arge *head, *tail, *cur;

    /* if no options specified in environment, we're done */
    if ((envopts = getenv("PGOPTION")) == (char *) NULL)
	return;

    if ((eopts = (char *) malloc(strlen(envopts) + 1)) == (char *) NULL) {
	fprintf(stderr, "cannot malloc copy space for PGOPTION\n");
	fflush(stderr);
	exit (2);
    }

    (void) strcpy(eopts, envopts);

    /*
     *  okay, we have PGOPTION from the environment, and we want to treat
     *  them as user-specified options.  to do this, we construct a new
     *  argv that has argv[0] followed by the arguments from the environment
     *  followed by the arguments on the command line.
     */

    head = cur = (arge *) NULL;
    neopts = 0;

    for (;;) {
	while (isspace(*eopts) && *eopts)
	    eopts++;

	if (*eopts == '\0')
	    break;

	if ((cur = (arge *) malloc(sizeof(arge))) == (arge *) NULL) {
	    fprintf(stderr, "cannot malloc space for arge\n");
	    fflush(stderr);
	    exit (2);
	}

	end = start = eopts;

	if (*start == '"') {
	    start++;
	    while (*++end != '\0' && *end != '"')
		continue;
	    if (*end == '\0') {
		fprintf(stderr, "unterminated string constant in env var PGOPTION\n");
		fflush(stderr);
		exit (2);
	    }
	    eopts = end + 1;
	} else if (*start == '\'') {
	    start++;
	    while (*++end != '\0' && *end != '\'')
		continue;
	    if (*end == '\0') {
		fprintf(stderr, "unterminated string constant in env var PGOPTION\n");
		fflush(stderr);
		exit (2);
	    }
	    eopts = end + 1;
	} else {
	    while (!isspace(*end) && *end)
		end++;
	    if (isspace(*end))
		eopts = end + 1;
	    else
		eopts = end;
	}

	if (head == (arge *) NULL) {
	    head = tail = cur;
	} else {
	    tail->a_next = cur;
	    tail = cur;
	}

	cur->a_arg = start;
	cur->a_next = (arge *) NULL;

	*end = '\0';
	neopts++;
    }

    argc = *argcP + neopts;

    if ((argv = (char **) malloc(argc * sizeof(char *))) == (char **) NULL) {
	fprintf(stderr, "can't malloc space for modified argv\n");
	fflush(stderr);
	exit (2);
    }

    curarg = argv;
    *curarg++ = *(*argvP)++;

    /* copy env args */
    while (head != (arge *) NULL) {
	cur = head;
	*curarg++ = head->a_arg;
	head = head->a_next;
	free(cur);
    }

    /* copy rest of args from command line */
    while (--(*argcP))
	*curarg++ = *(*argvP)++;

    /* all done */
    *argvP = argv;
    *argcP = argc;
}

static void
handle_copy_out(PGresult *res)
{
    bool copydone = false;
    char copybuf[COPYBUFSIZ];
    int ret;

    if (!Silent)
	fprintf(stdout, "Copy command returns...\n");
    
    while (!copydone) {
	ret = PQgetline(res->conn, copybuf, COPYBUFSIZ);
	
	if (copybuf[0] == '.' && copybuf[1] =='\0') {
	    copydone = true;	/* don't print this... */
	} else {
	    fputs(copybuf, stdout);
	    switch (ret) {
	    case EOF:
		copydone = true;
		/*FALLTHROUGH*/
	    case 0:
		fputc('\n', stdout);
		break;
	    case 1:
		break;
	    }
	}
    }
    fflush(stdout);
    PQendcopy(res->conn);
}

static void
handle_copy_in(PGresult *res)
{
    bool copydone = false;
    bool firstload;
    bool linedone;
    char copybuf[COPYBUFSIZ];
    char *s;
    int buflen;
    int c;
    
    if (!Silent) {
	fputs("Enter info followed by a newline\n", stdout);
	fputs("End with a dot on a line by itself.\n", stdout);
    }
    
    /*
     * eat inevitable newline still in input buffer
     *
     * XXX the 'inevitable newline' is not always present
     *     for example `cat file | monitor -c "copy from stdin"'
     */
    fflush(stdin);
    if ((c = getc(stdin)) != '\n' && c != EOF) {
	(void) ungetc(c, stdin);
    }
    
    while (!copydone) {			/* for each input line ... */
	if (!Silent) {
	    fputs(">> ", stdout);
	    fflush(stdout);
	}
	firstload = true;
	linedone = false;
	while (!linedone) {		/* for each buffer ... */
	    s = copybuf;
	    buflen = COPYBUFSIZ;
	    for (; buflen > 1 &&
		 !(linedone = (c = getc(stdin)) == '\n' || c == EOF);
		 --buflen) {
		*s++ = c;
	    }
	    if (c == EOF) {
		/* reading from stdin, but from a file */
		PQputline(res->conn, ".");
		copydone = true;
		break;
	    }
	    *s = '\0';
	    PQputline(res->conn, copybuf);
	    if (firstload) {
		if (!strcmp(copybuf, ".")) {
		    copydone = true;
		}
		firstload = false;
	    }
	}
	PQputline(res->conn, "\n");
    }
    PQendcopy(res->conn);
}
