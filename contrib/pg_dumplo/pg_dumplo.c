
#include <stdio.h>	
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>                            
#include <time.h>

#include <libpq-fe.h>
#include <libpq/libpq-fs.h>

#define _GNU_SOURCE
#include <getopt.h>
        
extern int 	errno;        
        
#define QUERY_BUFSIZ	(8*1024)
#define DIR_UMASK	0755
#define FILE_UMASK	0666 

#define	TRUE		1
#define FALSE		0
#define RE_OK		0
#define RE_ERROR	1

typedef struct { 
	char	*table,
		*attr;
	long	lo_oid;
} lo_attr;

void usage()
{
	printf("\nPostgreSQL large objects dump");
	printf("\npg_lo_dump <option>\n\n");
	printf("-h --help			this help\n");	
	printf("-u --user='username'		username for connection to server\n");	
	printf("-p --password='password'	password for connection to server\n");	
	printf("-d --db='database'		database name\n");	
	printf("-t --host='hostname'		server hostname\n");		
	printf("-l <table.attr ...>		dump attribute (columns) with LO to dump tree\n");
	printf("-i --import			import large obj dump tree to DB\n");
	printf("-s --space=<dir>		directory with dupm tree (for dump/import)\n");
	printf("\nExample (dump):   pg_lo_dump -d my_db -s /my_dump/dir/ -l t1.a t1.b t2.a\n");	
	printf("Example (import): pg_lo_dump -i -d my_db -s /my_dump/dir/\n");	
	printf("\nNote:  * option '-l' must be last option!\n");	
	printf("       * option '-i' (--import) make new large obj in DB, not rewrite old,\n");	
	printf("         import UPDATE oid numbers in table.attr only.\n");	
	printf("\n\n"); 
}

typedef enum {
	ARG_USER,
	ARG_PWD,
	ARG_DB,
	ARG_HELP,
	ARG_HOST
} _ARG_;

/*-----
 *	Init and allocate lo_attr structs
 *
 *	!	data is **argv 
 *-----
 */
lo_attr *init_loa(char **data, int max, int start)
{
	lo_attr 	*l, 
			*ll;
	char		**d, 
			*loc,
			buff[128];

	if ((l = (lo_attr *) malloc(max * sizeof(lo_attr))) == NULL) {
		fprintf(stderr, "%s: can't allocate memory\n", data[0]);
		exit(RE_ERROR);
	}
	for(d=data+start, ll=l; *d != NULL; d++, ll++) {
		strncpy(buff, *d, 128);
		if ((loc = strchr(buff, '.')) == NULL) {
			fprintf(stderr, "%s: '%s' is bad 'table.attr'\n", data[0], buff);
			exit(RE_ERROR);	
		}
		*loc = '\0';
		ll->table = strdup(buff);
		ll->attr = strdup(++loc);
	}
	ll++;
	ll->table = ll->attr = (char *) NULL;
	return l;
}

/*-----
 *	Check PG result
 *-----
 */
int check_res(PGresult *res, PGconn *conn) 
{
	if (!res && PQresultStatus(res) != PGRES_COMMAND_OK) {
        	fprintf(stderr, "%s\n",PQerrorMessage(conn));
                PQclear(res);
                return FALSE;
        }
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
                fprintf(stderr, "Tuples is not OK.\n");
                PQclear(res);
                return FALSE;
        }
        return TRUE;        
}


/*-----
 *	LO dump
 *-----
 */
 void dump_lo(PGconn *conn, char *space, lo_attr *loa, char *db, char *prog)
 {
	PGresult	*res;		
	lo_attr		*ploa;
	FILE		*majorfile;
	char		*dir,
			path[BUFSIZ],
			Qbuff[QUERY_BUFSIZ];
	
	dir = space ? space : getenv("PWD");		
	sprintf(path, "%s/%s", dir, db); 
	if (mkdir(path, DIR_UMASK) == -1) {
		if (errno != EEXIST) {
			perror(path);
			exit(RE_ERROR);					
		}	
	}
	
	sprintf(path, "%s/lo_dump.index", path);          
	if ((majorfile = fopen(path, "w")) == NULL) {
		perror(path);
		exit(RE_ERROR);
	} else {
		time_t	t;
		time(&t);
		fprintf(majorfile, "#\n# This is the PostgreSQL large object dump index\n#\n");
		fprintf(majorfile, "#\tDate:     %s", ctime(&t));
		fprintf(majorfile, "#\tHost:     %s\n", PQhost(conn) ? PQhost(conn) : "localhost");
		fprintf(majorfile, "#\tDatabase: %s\n", db);
		fprintf(majorfile, "#\tUser:     %s\n", PQuser(conn));
		fprintf(majorfile, "#\n# oid\ttable\tattribut\tinfile\n");
	}

	for(ploa=loa; ploa->table != NULL; ploa++) {
	
		/* query */
		sprintf(Qbuff, "SELECT %s FROM %s WHERE %s!=0", 
			ploa->attr, ploa->table, ploa->attr);
			
		res = PQexec(conn, Qbuff);
	
		if (check_res(res, conn)) {
			int	tuples	= PQntuples(res),
				t;
			char	*val;
	
			/* Make DIR/FILE */
			if (tuples) {
				sprintf(path, "%s/%s/%s", dir, db, ploa->table);          
				if (mkdir(path, DIR_UMASK) == -1) {
					if (errno != EEXIST) {
						perror(path);
						exit(RE_ERROR);					
					}	
				}
				sprintf(path, "%s/%s", path, ploa->attr);          
				if (mkdir(path, DIR_UMASK) == -1) {
					if (errno != EEXIST) {
						perror(path);
						exit(RE_ERROR);					
					}	
				}
				fprintf(stderr, "%s: dump %s.%s (%d lagre obj)\n", prog, 
					ploa->table, ploa->attr, tuples);
			}

			for(t=0; t<tuples; t++) {
				val = PQgetvalue(res, t, 0);
				if (!val)
					continue;
				
				sprintf(path, "%s/%s/%s/%s/%s", dir, db, ploa->table, ploa->attr, val);
				
				if (lo_export(conn, (Oid) atol(val), path) < 0) 
					fprintf(stderr, "%s\n", PQerrorMessage(conn));
				else
					fprintf(majorfile, "%s\t%s\t%s\t%s/%s/%s/%s\n", val, 
						ploa->table, ploa->attr, db, ploa->table, ploa->attr, val);
			}
		}
	}	
	fclose(majorfile);
 }


/*-----
 *	LO import
 *-----
 */
 void import_lo(PGconn *conn, char *space, char *db, char *prog)
 {
	PGresult	*res;		
	lo_attr		loa;
	FILE		*majorfile;
	long 		new_oid;
	char		*dir,
			tab[128], attr[128],
			path[BUFSIZ], lo_path[BUFSIZ],
			Qbuff[QUERY_BUFSIZ];
	
	dir = space ? space : getenv("PWD");
	sprintf(path, "%s/%s", dir, db);          

	sprintf(path, "%s/lo_dump.index", path);          
	if ((majorfile = fopen(path, "r")) == NULL) {
		perror(path);
		exit(RE_ERROR);
	} 

	while(fgets(Qbuff, QUERY_BUFSIZ, majorfile)) {
		
		if (*Qbuff == '#')
			continue;

		fprintf(stdout, Qbuff);
		
		sscanf(Qbuff, "%ld\t%s\t%s\t%s\n", &loa.lo_oid, tab, attr, path); 
		loa.table = tab;
		loa.attr  = attr;

		sprintf(lo_path, "%s/%s", dir, path); 
				
		/* import large obj */
		if ((new_oid = lo_import(conn, lo_path)) <= 0) {
			fprintf(stderr, "%s\n",PQerrorMessage(conn));
			PQexec(conn, "ROLLBACK");
			fprintf(stderr, "\nROLLBACK\n");
			exit(RE_ERROR);
		}
	
		/* query */
		sprintf(Qbuff, "UPDATE %s SET %s=%ld WHERE %s=%ld", 
			loa.table, loa.attr, new_oid, loa.attr, loa.lo_oid);

		/*fprintf(stderr, Qbuff);*/
			
		res = PQexec(conn, Qbuff);
	
		if (!res && PQresultStatus(res) != PGRES_COMMAND_OK) {
        		fprintf(stderr, "%s\n",PQerrorMessage(conn));
                	PQclear(res);
                	PQexec(conn, "ROLLBACK");
			fprintf(stderr, "\nROLLBACK\n");
			exit(RE_ERROR);
        	}	
        	
	}	
	fclose(majorfile);
 }

/*-----
 *	The mother of all C functions
 *-----
 */
int main(int argc, char **argv)
{
	PGconn		*conn;
	lo_attr		*loa	=NULL;
	char		*user	=NULL, 
			*pwd	=NULL, 
			*db	=NULL,
			*host	=NULL,	
			*space	=NULL;
	int		import	=FALSE;

	/* Parse argv */
	if (argc) {
      		int	arg, l_index=0;
      		extern int optind;
      		typedef enum {
			ARG_USER,
			ARG_PWD,
			ARG_DB,
			ARG_HELP,
			ARG_IMPORT,
			ARG_SPACE,
			ARG_HOST
		} _ARG_;
      		
      		struct option l_opt[] = {
      			{ "help",	0, 0, ARG_HELP	 	},		
			{ "user",	1, 0, ARG_USER 		},
			{ "pwd",	1, 0, ARG_PWD 		},
			{ "db",		1, 0, ARG_DB 		},
			{ "host",	1, 0, ARG_HOST 		},
			{ "space",	1, 0, ARG_SPACE		},
			{ "import",	0, 0, ARG_IMPORT	},
			{ NULL, 0, 0, 0 }
      		};     	

		while((arg = getopt_long(argc, argv, "hu:p:d:l:t:is:", l_opt, &l_index)) != -1) {
        		switch(arg) {
      			case ARG_HELP:
      			case 'h':
      				usage();
        		    	exit(RE_OK);
        		case ARG_USER:
        		case 'u':    	
        			user = strdup(optarg);
        			break;	 
        		case ARG_HOST:
        		case 't':    	
        			host = strdup(optarg);
        			break;	 	
        		case ARG_PWD:
        		case 'p':    	
        			pwd = strdup(optarg);
        			break;	          		
        		case ARG_DB:
        		case 'd':    	
        			db = strdup(optarg);
        			break;
        		case ARG_SPACE:
        		case 's':    	
        			space = strdup(optarg);
        			break;	
        		case ARG_IMPORT:
        		case 'i':    	
        			import = TRUE;
        			break;	
        		case 'l':
        			loa = init_loa(argv, argc-1, optind-1);
        			break;		        		
        		}
		}	
	}  	

	if (!space && !getenv("PWD")) {
		fprintf(stderr, "%s: can't set directory (not set '-s' or $PWD).\n", argv[0]);
		exit(RE_ERROR);
	}

	/* Make PG connection */
	conn = PQsetdbLogin(host, NULL, NULL, NULL, db, user, pwd);
        
	/* check to see that the backend connection was successfully made */
        if (PQstatus(conn) == CONNECTION_BAD) {
                fprintf(stderr, "%s\n",PQerrorMessage(conn));
                exit(RE_ERROR);
        }

	PQexec(conn, "BEGIN");

	if (import) {
		/* import obj */
		import_lo(conn, space, db, argv[0]);
	} else if (loa)	{
		/* Dump obj */
		dump_lo(conn, space, loa, db, argv[0]);
	} else {
		fprintf(stderr, "%s: ERROR: bad arg!\n", argv[0]);
		usage();
	}	

	PQexec(conn, "COMMIT");
	
	/* bye PG */ 
	PQfinish(conn);	
	exit(RE_OK);
}