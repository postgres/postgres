/* This program reads in an xbase-dbf file and sends 'inserts' to an
   PostgreSQL-server with the records in the xbase-file

   M. Boekhold (boekhold@cindy.et.tudelft.nl)  okt. 1995
   oktober 1996: merged sources of dbf2msql.c and dbf2pg.c
   oktober 1997: removed msql support
*/
#define HAVE_TERMIOS_H
#define HAVE_ICONV_H

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif

#include <libpq-fe.h>
#include "dbf.h"

int	verbose = 0, upper = 0, lower = 0, create = 0, fieldlow = 0;
int del = 0;
unsigned int begin = 0, end = 0;
unsigned int t_block = 0;
#ifdef HAVE_ICONV_H
char *charset_from=NULL;
char *charset_to="ISO-8859-1";
iconv_t iconv_d;
char convert_charset_buff[8192];
#endif

char	*host = NULL;
char 	*dbase = "test";
char 	*table = "test";
char	*username = NULL;
char	*password = NULL;
char	*subarg = NULL;
char	escape_buff[8192];

void do_substitute(char *subarg, dbhead *dbh);
inline void strtoupper(char *string);

inline void strtolower(char *string);
void do_create(PGconn *, char*, dbhead*);
void do_inserts(PGconn *, char*, dbhead*);
int check_table(PGconn *, char*);

char *Escape(char*);
#ifdef HAVE_ICONV_H
char *convert_charset(char *string);
#endif
void usage(void);
unsigned int isinteger(char *);

char *simple_prompt(const char *prompt, int maxlen, int echo);


unsigned int isinteger(char *buff) {
	char *i=buff;

	while (*i != '\0') {
		if (i==buff)
			if ((*i == '-') ||
				(*i == '+')) {
				i++; continue;
			}
		if (!isdigit((int)*i)) return 0;
		i++;
	}
	return 1;
}

inline void strtoupper(char *string) {
	while(*string != '\0') {
		*string = toupper(*string);
		string++;
	}
}

inline void strtolower(char *string) {
	while(*string != '\0') {
		*string = tolower(*string);
		string++;
	}
}

/* FIXME: should this check for overflow? */
char *Escape(char *string) {
    char    *foo, *bar;

    foo = escape_buff;

    bar = string;
    while (*bar != '\0') {
        if ((*bar == '\t') ||
        	(*bar == '\n') ||
        	(*bar == '\\')) {
            *foo++ = '\\';
        }
        *foo++ = *bar++;
    }
    *foo = '\0';

    return escape_buff;
}

#ifdef HAVE_ICONV_H
char *convert_charset(char *string) {
	size_t in_size, out_size, nconv;
	char *in_ptr,*out_ptr;

	in_size=strlen(string)+1;
	out_size=sizeof(convert_charset_buff);
	in_ptr=string;
	out_ptr=convert_charset_buff;

	iconv(iconv_d, NULL, &in_size, &out_ptr, &out_size); /* necessary to reset state information */
	while(in_size>0)
	{
		nconv = iconv(iconv_d, &in_ptr, &in_size, &out_ptr, &out_size);
		if(nconv == (size_t) -1)
		{
			printf("WARNING: cannot convert charset of string \"%s\".\n",
				string);
			strcpy(convert_charset_buff,string);
			return convert_charset_buff;
		}
	}
	*out_ptr = 0; /* terminate output string */
	return convert_charset_buff;
}
#endif

int check_table(PGconn *conn, char *table) {
	char		*q = "select relname from pg_class where "
					"relkind='r' and relname !~* '^pg'";
	PGresult	*res;
	int 		i = 0;

	if (!(res = PQexec(conn, q))) {
		printf("%s\n", PQerrorMessage(conn));
		return 0;
	}

	for (i = 0; i < PQntuples(res); i++) {
		if (!strcmp(table, PQgetvalue(res, i, PQfnumber(res, "relname")))) {
			return 1;
		}
	}

	return 0;
}

void usage(void){
	printf("\
dbf2pg
usage: dbf2pg [-u | -l] [-h hostname] [-W] [-U username]
              [-B transaction_size] [-F charset_from [-T charset_to]]
              [-s oldname=newname[,oldname=newname[...]]] [-d dbase]
              [-t table] [-c | -D] [-f] [-v[v]] dbf-file\n");
}

/* patch submitted by Jeffrey Y. Sue <jysue@aloha.net> */
/* Provides functionallity for substituting dBase-fieldnames for others */
/* Mainly for avoiding conflicts between fieldnames and SQL-reserved */
/* keywords */

void do_substitute(char *subarg, dbhead *dbh)
{
      /* NOTE: subarg is modified in this function */
      int i,bad;
      char *p,*oldname,*newname;
      if (!subarg) {
              return;
      }
      if (verbose>1) {
              printf("Substituting new field names\n");
      }
      /* use strstr instead of strtok because of possible empty tokens */
      oldname = subarg;
      while (oldname && strlen(oldname) && (p=strstr(oldname,"=")) ) {
              *p = '\0';      /* mark end of oldname */
              newname = ++p;  /* point past \0 of oldname */
              if (strlen(newname)) {  /* if not an empty string */
                      p = strstr(newname,",");
                      if (p) {
                              *p = '\0';      /* mark end of newname */
                              p++;    /* point past where the comma was */
                      }
              }
              if (strlen(newname)>=DBF_NAMELEN) {
                      printf("Truncating new field name %s to %d chars\n",
                              newname,DBF_NAMELEN-1);
                      newname[DBF_NAMELEN-1] = '\0';
              }
              bad = 1;
              for (i=0;i<dbh->db_nfields;i++) {
                      if (strcmp(dbh->db_fields[i].db_name,oldname)==0) {
                              bad = 0;
                              strcpy(dbh->db_fields[i].db_name,newname);
                              if (verbose>1) {
                                      printf("Substitute old:%s new:%s\n",
                                              oldname,newname);
                              }
                              break;
                      }
              }
              if (bad) {
                      printf("Warning: old field name %s not found\n",
                              oldname);
              }
              oldname = p;
      }
} /* do_substitute */

void do_create(PGconn *conn, char *table, dbhead *dbh) {
	char		*query;
	char 		t[20];
	int			i, length;
	PGresult	*res;

	if (verbose > 1) {
		printf("Building CREATE-clause\n");
	}

	if (!(query = (char *)malloc(
			(dbh->db_nfields * 40) + 29 + strlen(table)))) {
		fprintf(stderr, "Memory allocation error in function do_create\n");
		PQfinish(conn);
		close(dbh->db_fd);
		free(dbh);
		exit(1);
	}

	sprintf(query, "CREATE TABLE %s (", table);
	length = strlen(query);
	for ( i = 0; i < dbh->db_nfields; i++) {
              if (!strlen(dbh->db_fields[i].db_name)) {
                      continue;
                      /* skip field if length of name == 0 */
              }
		if ((strlen(query) != length)) {
                        strcat(query, ",");
                }

				if (fieldlow)
					strtolower(dbh->db_fields[i].db_name);

                strcat(query, dbh->db_fields[i].db_name);
                switch(dbh->db_fields[i].db_type) {
						case 'D':
								strcat(query, " date");
								break;
                        case 'C':
								if (dbh->db_fields[i].db_flen > 1) {
									strcat(query, " varchar");
									sprintf(t, "(%d)",
											dbh->db_fields[i].db_flen);
									strcat(query, t);
								} else {
	                                strcat(query, " char");
								}
                                break;
                        case 'N':
                                if (dbh->db_fields[i].db_dec != 0) {
                                        strcat(query, " real");
                                } else {
                                        strcat(query, " int");
                                }
                                break;
                        case 'L':
                                strcat(query, " char");
                                break;
                }
	}

	strcat(query, ")");

	if (verbose > 1) {
		printf("Sending create-clause\n");
		printf("%s\n", query);
	}

	if ((res = PQexec(conn, query)) == NULL) {
		fprintf(stderr, "Error creating table!\n");
		fprintf(stderr, "Detailed report: %s\n", PQerrorMessage(conn));
                close(dbh->db_fd);
                free(dbh);
				free(query);
                PQfinish(conn);
                exit(1);
	}

	PQclear(res);
	free(query);
}

/* FIXME: can be optimized to not use strcat, but it is worth the effort? */
void do_inserts(PGconn *conn, char *table, dbhead *dbh) {
	PGresult	*res;
	field		*fields;
	int			i, h, result;
	char		*query, *foo;
	char		pgdate[10];

	if (verbose > 1) {
		printf("Inserting records\n");
	}

	h = 2; /* 2 because of terminating \n\0 */

	for ( i = 0 ; i < dbh->db_nfields ; i++ ) {
		h += dbh->db_fields[i].db_flen > 2 ?
					dbh->db_fields[i].db_flen :
					2; /* account for possible NULL values (\N) */
		h += 1; /* the delimiter */
	}

	/* make sure we can build the COPY query, note that we don't need to just
 	add this value, since the COPY query is a separate query (see below) */
	if (h < 17+strlen(table)) h = 17+strlen(table);

	if (!(query = (char *)malloc(h))) {
		PQfinish(conn);
		fprintf(stderr,
			"Memory allocation error in function do_inserts (query)\n");
		close(dbh->db_fd);
		free(dbh);
		exit(1);
	}

	if ((fields = dbf_build_record(dbh)) == (field *)DBF_ERROR) {
        fprintf(stderr,
            "Couldn't allocate memory for record in do_insert\n");
		PQfinish(conn);
        free(query);
        dbf_close(dbh);
        exit(1);
    }
		
	if (end == 0)              /* "end" is a user option, if not specified, */
		end = dbh->db_records; /* then all records are processed. */

	if (t_block == 0) /* user not specified transaction block size */
		t_block = end-begin; /* then we set it to be the full data */
		
	for (i = begin; i < end; i++) {
		/* we need to start a new transaction and COPY statement */
		if (((i-begin) % t_block) == 0) {
			if (verbose > 1)
				fprintf(stderr, "Transaction: START\n");
			res = PQexec(conn, "BEGIN");
			if (res == NULL) {
				fprintf(stderr, "Error starting transaction!\n");
				fprintf(stderr, "Detailed report: %s\n", PQerrorMessage(conn));
                exit(1);
			}
			sprintf(query, "COPY %s FROM stdin", table);
			res = PQexec(conn, query);
			if (res == NULL) {
				fprintf(stderr, "Error starting COPY!\n");
				fprintf(stderr, "Detailed report: %s\n", PQerrorMessage(conn));
                exit(1);
			}
		}

		/* build line and submit */
		result = dbf_get_record(dbh, fields,  i);
		if (result == DBF_VALID) {
			query[0] = '\0';
			for (h = 0; h < dbh->db_nfields; h++) {
				if (!strlen(fields[h].db_name)) {
					continue;
				}

				if (h!=0) /* not for the first field! */
					strcat(query, "\t"); /* COPY statement field separator */

				if (upper) {
				   strtoupper(fields[h].db_contents);
				}
				if (lower) {
					strtolower(fields[h].db_contents);
				}

				foo = fields[h].db_contents;
#ifdef HAVE_ICONV_H
				if(charset_from)
					foo = convert_charset(foo);
#endif
				foo = Escape(foo);

				/* handle the date first - liuk */
				if(fields[h].db_type=='D') {	
					if((strlen(foo)==8) && isinteger(foo)) {
						sprintf(pgdate,"%c%c%c%c-%c%c-%c%c",
							foo[0],foo[1],foo[2],foo[3],
							foo[4],foo[5],foo[6],foo[7]);
						strcat(query,pgdate);
					} else {
						/* empty field must be inserted as NULL value in this
 						   way */
						strcat(query,"\\N");
					}
				}
				else if ((fields[h].db_type == 'N') &&
					(fields[h].db_dec == 0)){
					if (isinteger(foo)) {
						strcat(query, foo);
					} else {
						strcat(query, "\\N");
						if (verbose)
							fprintf(stderr, "Illegal numeric value found "
											"in record %d, field \"%s\"\n",
											i, fields[h].db_name);
					}
				} else {
					strcat(query, foo); /* must be character */
				}
			}
			strcat(query, "\n");

			if ((verbose > 1) && (( i % 100) == 0)) {/* Only show every 100 */
				printf("Inserting record %d\n", i);  /* records. */
			}
			PQputline(conn, query);

		}
		/* we need to end this copy and transaction */
		if (((i-begin) % t_block) == t_block-1) {
			if (verbose > 1)
				fprintf(stderr, "Transaction: END\n");
			PQputline(conn, "\\.\n");
			if (PQendcopy(conn) != 0) {
				fprintf(stderr, "Something went wrong while copying. Check "
								"your tables!\n");
				exit(1);
			}
			res = PQexec(conn, "END");
			if (res == NULL) {
				fprintf(stderr, "Error committing work!\n");
				fprintf(stderr, "Detailed report: %s\n", PQerrorMessage(conn));
				exit(1);
			}
		}
	}

	/* last row copied in, end copy and transaction */
	/* remember, i is now 1 greater then when we left the loop */
	if (((i-begin) % t_block) != 0) {
		if (verbose > 1)
			fprintf(stderr, "Transaction: END\n");
		PQputline(conn, "\\.\n");

		if (PQendcopy(conn) != 0) {
			fprintf(stderr, "Something went wrong while copying. Check "
							"your tables!\n");
		}
		res = PQexec(conn, "END");
		if (res == NULL) {
			fprintf(stderr, "Error committing work!\n");
			fprintf(stderr, "Detailed report: %s\n", PQerrorMessage(conn));
			exit(1);
		}
	}
	dbf_free_record(dbh, fields);

	free(query);
}

/*
 * This is from Postgres 7.0.3 source tarball, utility program PSQL.
 *
 * simple_prompt
 *
 * Generalized function especially intended for reading in usernames and
 * password interactively. Reads from stdin.
 *
 * prompt:		The prompt to print
 * maxlen:		How many characters to accept
 * echo:		Set to false if you want to hide what is entered (for passwords)
 *
 * Returns a malloc()'ed string with the input (w/o trailing newline).
 */
static int prompt_state;

char *
simple_prompt(const char *prompt, int maxlen, int echo)
{
	int			length;
	char	   *destination;

#ifdef HAVE_TERMIOS_H
	struct termios t_orig,
				t;

#endif

	destination = (char *) malloc(maxlen + 2);
	if (!destination)
		return NULL;
	if (prompt)
		fputs(prompt, stderr);

	prompt_state = 1;

#ifdef HAVE_TERMIOS_H
	if (!echo)
	{
		tcgetattr(0, &t);
		t_orig = t;
		t.c_lflag &= ~ECHO;
		tcsetattr(0, TCSADRAIN, &t);
	}
#endif

	fgets(destination, maxlen, stdin);

#ifdef HAVE_TERMIOS_H
	if (!echo)
	{
		tcsetattr(0, TCSADRAIN, &t_orig);
		puts("");
	}
#endif

	prompt_state = 0;

	length = strlen(destination);
	if (length > 0 && destination[length - 1] != '\n')
	{
		/* eat rest of the line */
		char		buf[512];

		do
		{
			fgets(buf, 512, stdin);
		} while (buf[strlen(buf) - 1] != '\n');
	}

	if (length > 0 && destination[length - 1] == '\n')
		/* remove trailing newline */
		destination[length - 1] = '\0';

	return destination;
}


int main(int argc, char **argv)
{
	PGconn 		*conn;
	int			i;
	extern int 	optind;
	extern char	*optarg;
	char		*query;
	dbhead		*dbh;

	while ((i = getopt(argc, argv, "DWflucvh:b:e:d:t:s:B:U:F:T:")) != EOF) {
		switch (i) {
			case 'D':
				if (create) {
					usage();
					printf("Can't use -c and -D at the same time!\n");
					exit(1);
				}
				del = 1;
				break;
			case 'W':
				password=simple_prompt("Password: ",100,0);
				break;
			case 'f':
				fieldlow=1;
				break;
			case 'v':
				verbose++;
				break;
			case 'c':
				if (del) {
					usage();
					printf("Can't use -c and -D at the same time!\n");
					exit(1);
				}
				create=1;
				break;
			case 'l':
				lower=1;
				break;
			case 'u':
				if (lower) {
					usage();
					printf("Can't use -u and -l at the same time!\n");
					exit(1);
				}
				upper=1;
				break;
			case 'b':
				begin = atoi(optarg);
				break;
			case 'e':
				end = atoi(optarg);
				break;
			case 'h':
				host = (char *)strdup(optarg);
				break;
			case 'd':
				dbase = (char *)strdup(optarg);
				break;
			case 't':
				table = (char *)strdup(optarg);
                break;
			case 's':
				subarg = (char *)strdup(optarg);
				break;
			case 'B':
				t_block = atoi(optarg);
				break;
			case 'U':
				username = (char *)strdup(optarg);
				break;
			case 'F':
				charset_from = (char *)strdup(optarg);
				break;
			case 'T':
				charset_to = (char *)strdup(optarg);
				break;
			case ':':
				usage();
				printf("missing argument!\n");
				exit(1);
				break;
			case '?':
				usage();
				/* FIXME: Ivan thinks this is bad: printf("unknown argument: %s\n", argv[0]); */
				exit(1);
				break;
			default:
				break;
		}
	}

	argc -= optind;
	argv = &argv[optind];

	if (argc != 1) {
		usage();
		if(username)
			free(username);
		if(password)
			free(password);
		exit(1);
	}

#ifdef HAVE_ICONV_H
	if(charset_from)
	{
		if(verbose>1)
			printf("Setting conversion from charset \"%s\" to \"%s\".\n",
				charset_from,charset_to);
		iconv_d = iconv_open(charset_to,charset_from);
		if(iconv_d == (iconv_t) -1)
		{
			printf("Cannot convert from charset \"%s\" to charset \"%s\".\n",
				charset_from,charset_to);
			exit(1);
		}
	}
#endif

	if (verbose > 1) {
		printf("Opening dbf-file\n");
	}

	if ((dbh = dbf_open(argv[0], O_RDONLY)) == (dbhead *)-1) {
		fprintf(stderr, "Couldn't open xbase-file %s\n", argv[0]);
		if(username)
			free(username);
		if(password)
			free(password);
		if(charset_from)
			iconv_close(iconv_d);
		exit(1);
	}

	if (fieldlow)
		for ( i = 0 ; i < dbh->db_nfields ; i++ )
				strtolower(dbh->db_fields[i].db_name);

	if (verbose) {
		printf("dbf-file: %s, PG-dbase: %s, PG-table: %s\n", argv[0],
																 dbase,
																 table);
		printf("Number of records: %ld\n", dbh->db_records);
		printf("NAME:\t\tLENGTH:\t\tTYPE:\n");
		printf("-------------------------------------\n");
		for (i = 0; i < dbh->db_nfields ; i++) {
			printf("%-12s\t%7d\t\t%5c\n",dbh->db_fields[i].db_name,
									 dbh->db_fields[i].db_flen,
									 dbh->db_fields[i].db_type);
		}
	}

	if (verbose > 1) {
		printf("Making connection to PG-server\n");
	}

	conn = PQsetdbLogin(host,NULL,NULL,NULL, dbase, username, password);
	if (PQstatus(conn) != CONNECTION_OK) {
		fprintf(stderr, "Couldn't get a connection with the ");
		fprintf(stderr, "designated host!\n");
		fprintf(stderr, "Detailed report: %s\n", PQerrorMessage(conn));
		close(dbh->db_fd);
		free(dbh);
		if(username)
			free(username);
		if(password)
			free(password);
		if(charset_from)
			iconv_close(iconv_d);
		exit(1);
	}

/* Substitute field names */
	do_substitute(subarg, dbh);

/* create table if specified, else check if target table exists */
	if (!create) {
		if (!check_table(conn, table)) {
			printf("Table does not exist!\n");
			if(username)
				free(username);
			if(password)
				free(password);
			if(charset_from)
				iconv_close(iconv_d);
			exit(1);
		}
		if (del) {
			if (!(query = (char *)malloc(13 + strlen(table)))) {
				printf("Memory-allocation error in main (delete)!\n");
				close(dbh->db_fd);
				free(dbh);
				PQfinish(conn);
				if(username)
					free(username);
				if(password)
					free(password);
				if(charset_from)
					iconv_close(iconv_d);
				exit(1);
			}
			if (verbose > 1) {
				printf("Deleting from original table\n");
			}
			sprintf(query, "DELETE FROM %s", table);
			PQexec(conn, query);
			free(query);
		}
	} else {
		if (!(query = (char *)malloc(12 + strlen(table)))) {
			printf("Memory-allocation error in main (drop)!\n");
			close(dbh->db_fd);
			free(dbh);
			PQfinish(conn);
			if(username)
				free(username);
			if(password)
				free(password);
			if(charset_from)
				iconv_close(iconv_d);
			exit(1);
		}
		if (verbose > 1) {
			printf("Dropping original table (if one exists)\n");
		}
		sprintf(query, "DROP TABLE %s", table);
		PQexec(conn, query);
		free(query);

/* Build a CREATE-clause
*/
		do_create(conn, table, dbh);
	}

/* Build an INSERT-clause
*/
	PQexec(conn, "SET DATESTYLE TO 'ISO';");
	do_inserts(conn, table, dbh);

	if (verbose > 1) {
		printf("Closing up....\n");
	}

    close(dbh->db_fd);
    free(dbh);
    PQfinish(conn);
	if(username)
		free(username);
	if(password)
		free(password);
	if(charset_from)
		iconv_close(iconv_d);
	exit(0);
}
