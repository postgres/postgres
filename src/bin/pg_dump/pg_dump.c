/*-------------------------------------------------------------------------
 *
 * pg_dump.c--
 *    pg_dump is an utility for dumping out a postgres database
 * into a script file.
 *
 *  pg_dump will read the system catalogs in a database and 
 *  dump out a script that reproduces
 *  the schema of the database in terms of
 *        user-defined types
 *        user-defined functions
 *        tables
 *        indices
 *        aggregates
 *        operators
 *        ACL - grant/revoke
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/pg_dump/pg_dump.c,v 1.30 1997/06/05 22:59:45 momjian Exp $
 *
 * Modifications - 6/10/96 - dave@bensoft.com - version 1.13.dhb
 *
 *   Applied 'insert string' patch from "Marc G. Fournier" <scrappy@ki.net>
 *   Added '-t table' option
 *   Added '-a' option
 *   Added '-da' option
 *
 * Modifications - 6/12/96 - dave@bensoft.com - version 1.13.dhb.2
 *
 *   - Fixed dumpTable output to output lengths for char and varchar types!
 *   - Added single. quote to twin single quote expansion for 'insert' string
 *     mode.
 *
 * Modifications - 7/26/96 - asussman@vidya.com
 *
 *   - Fixed ouput lengths for char and varchar type where the length is variable (-1)
 *
 * Modifications - 6/1/97 - igor@sba.miami.edu
 * - Added functions to free allocated memory used for retrieving
 *   indices,tables,inheritance,types,functions and aggregates.
 *   No more leaks reported by Purify.
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <unistd.h>     /* for getopt() */
#include <stdio.h>
#include <string.h>
#include <sys/param.h>  /* for MAXHOSTNAMELEN on most */
#ifdef sparc_solaris
#include <netdb.h>      /* for MAXHOSTNAMELEN on some */
#endif

#include "postgres.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "catalog/pg_index.h"
#include "libpq-fe.h"
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "pg_dump.h"

static void dumpSequence (FILE* fout, TableInfo tbinfo);

extern char *optarg;
extern int optind, opterr;

/* global decls */
bool g_verbose;  /* User wants verbose narration of our activities. */
int g_last_builtin_oid; /* value of the last builtin oid */
FILE *g_fout;     /* the script file */
PGconn *g_conn;   /* the database connection */
int dumpData; /* dump data using proper insert strings */
int attrNames; /* put attr names into insert strings */
int schemaOnly;
int dataOnly;

char g_opaque_type[10]; /* name for the opaque type */

/* placeholders for the delimiters for comments */
char g_comment_start[10]; 
char g_comment_end[10]; 


static void
usage(const char* progname)
{
    fprintf(stderr, 
            "%s - version 1.13.dhb.2\n\n",progname);
    fprintf(stderr, 
            "usage:  %s [options] [dbname]\n",progname);
    fprintf(stderr, 
            "\t -f filename \t\t script output filename\n");
    fprintf(stderr, 
            "\t -H hostname \t\t server host name\n");
    fprintf(stderr, 
            "\t -p port     \t\t server port number\n");
    fprintf(stderr, 
            "\t -v          \t\t verbose\n");
    fprintf(stderr, 
            "\t -d          \t\t dump data as proper insert strings\n");
    fprintf(stderr, 
            "\t -D          \t\t dump data as inserts with attribute names\n");
    fprintf(stderr, 
            "\t -S          \t\t dump out only the schema, no data\n");
    fprintf(stderr, 
            "\t -a          \t\t dump out only the data, no schema\n");
    fprintf(stderr, 
            "\t -t table    \t\t dump for this table only\n");
    fprintf(stderr, 
            "\t -o          \t\t dump object id's (oids)\n");
    fprintf(stderr, 
            "\t -z          \t\t dump ACLs (grant/revoke)\n");
    fprintf(stderr, 
            "\nIf dbname is not supplied, then the DATABASE environment "
            "variable value is used.\n");
    fprintf(stderr, "\n");

    exit(1);
}

static void 
exit_nicely(PGconn* conn)
{
  PQfinish(conn);
  exit(1);
}


/*
 * isViewRule
 *		Determine if the relation is a VIEW 
 *
 */
bool
isViewRule(char *relname)
{
    PGresult *res;
    int ntups;
    char query[MAXQUERYLEN];
    
    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query, "select relname from pg_class, pg_rewrite "
		   "where pg_class.oid = ev_class "
		   "and rulename = '_RET%s'", relname);

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"isViewRule(): SELECT failed\n");
        exit_nicely(g_conn);
    }
    
    ntups = PQntuples(res);

    PQclear(res);
    res = PQexec(g_conn, "end");
    PQclear(res);
    return ntups > 0 ? TRUE : FALSE;
}

#define COPYBUFSIZ      8192


static void 
dumpClasses_nodumpData(FILE *fout, const char *classname, const bool oids) {

    PGresult *res;
    char query[255];
    int ret;
    bool copydone;
    char copybuf[COPYBUFSIZ];

    if (oids) {
        fprintf(fout, "COPY %s WITH OIDS FROM stdin;\n", 
                classname);
        sprintf(query, "COPY %s WITH OIDS TO stdout;\n", 
                classname);
    } else {
        fprintf(fout, "COPY %s FROM stdin;\n", classname);
        sprintf(query, "COPY %s TO stdout;\n", classname);
    }
    res = PQexec(g_conn, query);
    if (!res) {
        fprintf(stderr, "SQL query to dump the contents of Table %s "
                "did not execute.  Explanation from backend: '%s'.\n"
                "The query was: '%s'.\n",
                classname, PQerrorMessage(g_conn), query);
        exit_nicely(g_conn);
    } else {
        if (PQresultStatus(res) != PGRES_COPY_OUT) {
            fprintf(stderr,"SQL query to dump the contents of Table %s "
                    "executed abnormally.\n"
                    "PQexec() returned status %d when %d was expected.\n"
                    "The query was: '%s'.\n",
                    classname, PQresultStatus(res), PGRES_COPY_OUT, query);
            exit_nicely(g_conn);
        } else {
            copydone = false;
            while (!copydone) {
                ret = PQgetline(res->conn, copybuf, COPYBUFSIZ); 
               
                if (copybuf[0] == '\\' &&
                    copybuf[1] == '.' &&
                    copybuf[2] == '\0') {
                    copydone = true;        /* don't print this... */
                } else {
                    fputs(copybuf, fout);
                    switch (ret) {
                      case EOF:
                        copydone = true;
                        /*FALLTHROUGH*/
                      case 0:
                        fputc('\n', fout);
                        break;
                      case 1:
                        break;
                    }
                }
            }
            fprintf(fout, "\\.\n");
        }
        ret = PQendcopy(res->conn);
        if (ret != 0) {
            fprintf(stderr, "SQL query to dump the contents of Table %s "
                    "did not execute correctly.  After we read all the "
                    "table contents from the backend, PQendcopy() failed.  "
                    "Explanation from backend: '%s'.\n"
                    "The query was: '%s'.\n",
                    classname, PQerrorMessage(g_conn), query);
            if(res) PQclear(res);
            exit_nicely(g_conn);
        }
        if(res) PQclear(res);
    }
}



static void
dumpClasses_dumpData(FILE *fout, const char *classname, 
                     const TableInfo tblinfo, bool oids) {        

    PGresult *res;
    char query[255];
    int actual_atts; /* number of attrs in this a table */
    char expandbuf[COPYBUFSIZ];
    char q[MAXQUERYLEN];
    int tuple;
    int field;

    sprintf(query, "select * from %s;\n", classname);
    res = PQexec(g_conn, query);
    if (!res ||
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"dumpClasses(): command failed\n");
        exit_nicely(g_conn);
    }
    tuple=0;
    while(tuple < PQntuples(res)) {
        fprintf(fout, "insert into %s ", classname);
        if (attrNames) {
            int j;
            actual_atts = 0;
            sprintf(q, "(");
            for (j=0;j<tblinfo.numatts;j++) {
                if (tblinfo.inhAttrs[j] == 0) {
                    sprintf(q, "%s%s%s",
                            q,
                            (actual_atts > 0) ? "," : "",
                            tblinfo.attnames[j]);
                    actual_atts++;
                }
            }
            sprintf(q,"%s%s",q, ") ");
            fprintf(fout, q);
        }
        fprintf(fout, "values (");
        field=0;
        do {
            if (PQgetisnull(res,tuple,field)) {
                fprintf(fout,"NULL");
            } else {
                switch(PQftype(res,field)) {
                  case INT2OID: case INT4OID: case OIDOID: /* int types */
                  case FLOAT4OID: case FLOAT8OID: /* float types */
                    fprintf(fout, "%s",
                            PQgetvalue(res,tuple,field));
                    break;
                  default: {
                      char *expsrc,*expdest;
  
                    /* Before outputting string value, expand all
                       single quotes to twin single quotes -
                       dhb - 6/11/96 */
                    expsrc=PQgetvalue(res,tuple,field);
                      expdest=expandbuf;
                      while (*expsrc) {
                          *expdest++=*expsrc;
                          if (*expsrc == (char)0x27)  /*single quote*/
                            *expdest++ = *expsrc;
                          expsrc++;
                      }
                      *expdest=*expsrc; /* null term. */
                      
                      fprintf(fout, "'%s'", expandbuf);
                  }
                    break;
                }
            }
            field++;
            if(field != PQnfields(res))
              fprintf(fout, ",");
        } while(field < PQnfields(res));
        fprintf(fout, ");\n");
        tuple++;
    }
    PQclear(res);
}



/*
 * DumpClasses -
 *    dump the contents of all the classes.
 */
static void
dumpClasses(const TableInfo tblinfo[], const int numTables, FILE *fout, 
            const char *onlytable, const bool oids) {

    int i;
    char *all_only;

    if (onlytable == NULL) all_only = "all";
    else all_only = "one";
    
    if (g_verbose)
      fprintf(stderr, "%s dumping out the contents of %s of %d tables %s\n",
              g_comment_start, all_only, numTables, g_comment_end);
    
    for(i = 0; i < numTables; i++) {
        const char *classname = tblinfo[i].relname;

	/* Skip VIEW relations */
	if (isViewRule(tblinfo[i].relname))
		continue;

        if (!onlytable || (!strcmp(classname,onlytable)))
        {
            if ( tblinfo[i].sequence )
            {
            	if ( dataOnly )		/* i.e. SCHEMA didn't dumped */
            	{
            	    if ( g_verbose )
              	    	fprintf (stderr, "%s dumping out schema of sequence %s %s\n",
				g_comment_start, classname, g_comment_end);
		    dumpSequence (fout, tblinfo[i]);
		}
		else if ( g_verbose )
              	    fprintf (stderr, "%s contents of sequence '%s' dumped in schema %s\n",
				g_comment_start, classname, g_comment_end);
		continue;
	    }
        
            if (g_verbose)
              fprintf(stderr, "%s dumping out the contents of Table %s %s\n",
                      g_comment_start, classname, g_comment_end);

            /* skip archive names*/
            if (isArchiveName(classname))
              continue;

            if(!dumpData) 
              dumpClasses_nodumpData(fout, classname, oids);
            else 
              dumpClasses_dumpData(fout, classname, tblinfo[i], oids);
        }
    }        
}



int
main(int argc, char** argv)
{
    int c;
    const char* progname;
    const char* filename = NULL;
    const char* dbname = NULL;
    const char *pghost = NULL;
    const char *pgport = NULL;
    const char *tablename = NULL;
    int oids = 0, acls = 0;
    TableInfo *tblinfo;
    int numTables;

    g_verbose = false;
    
    strcpy(g_comment_start,"-- ");
    g_comment_end[0] = '\0';
    strcpy(g_opaque_type, "opaque");

    dataOnly = schemaOnly = dumpData = attrNames = 0;

    progname = *argv;

    while ((c = getopt(argc, argv,"f:H:p:t:vSDdDaoz")) != EOF) {
        switch(c) {
        case 'f': /* output file name */
            filename = optarg;
            break;
        case 'H' : /* server host */
            pghost = optarg;
            break;
        case 'p' : /* server port */
            pgport = optarg;
            break;
        case 'v': /* verbose */
            g_verbose = true;
            break;
        case 'S': /* dump schema only */
            schemaOnly = 1;
            break;
        case 'd': /* dump data as proper insert strings */
            dumpData = 1;
            break;
        case 'D': /* dump data as proper insert strings with attr names */
            dumpData = 1;
            attrNames = 1;
            break;
        case 't': /* Dump data for this table only */
            tablename = optarg;
            break;
        case 'a': /* Dump data only */
            dataOnly = 1;
            break;
        case 'o': /* Dump oids */
            oids = 1;
            break;
        case 'z': /* Dump oids */
            acls = 1;
            break;
        default:
            usage(progname);
            break;
        }
    }

    /* open the output file */
    if (filename == NULL) {
        g_fout = stdout;
    } else {
        g_fout = fopen(filename, "w");
        if (g_fout == NULL) {
            fprintf(stderr,
                    "%s: could not open output file named %s for writing\n",
                    progname, filename);
            exit(2);
        }
    }

    /* find database */
    if (!(dbname = argv[optind]) &&
        !(dbname = getenv("DATABASE")) ) {
            fprintf(stderr, "%s: no database name specified\n",progname);
            exit (2);
        }

    g_conn = PQsetdb(pghost, pgport, NULL, NULL, dbname);
    /* check to see that the backend connection was successfully made */
    if (PQstatus(g_conn) == CONNECTION_BAD) {
        fprintf(stderr,"Connection to database '%s' failed.\n", dbname);
        fprintf(stderr,"%s\n",PQerrorMessage(g_conn));
        exit_nicely(g_conn);
    }

    g_last_builtin_oid = findLastBuiltinOid();

    if (oids)
        setMaxOid(g_fout);
    if (!dataOnly) {
        if (g_verbose) 
            fprintf(stderr, "%s last builtin oid is %d %s\n",
                    g_comment_start,  g_last_builtin_oid, g_comment_end);
        tblinfo = dumpSchema(g_fout, &numTables, tablename, acls);
    }
    else
      tblinfo = dumpSchema(NULL, &numTables, tablename, acls);
    
    if (!schemaOnly) {
      dumpClasses(tblinfo, numTables, g_fout, tablename, oids);
    }     

    if (!dataOnly) /* dump indexes at the end for performance */
        dumpSchemaIdx(g_fout, &numTables, tablename, tblinfo, numTables);
    
    fflush(g_fout);
    fclose(g_fout);
    clearTableInfo(tblinfo, numTables);
    PQfinish(g_conn);
    exit(0);
}

/*
 * getTypes: 
 *    read all base types in the system catalogs and return them in the 
 * TypeInfo* structure
 *
 *  numTypes is set to the number of types read in 
 *
 */
TypeInfo*
getTypes(int *numTypes)
{
    PGresult *res;
    int ntups;
    int i;
    char query[MAXQUERYLEN];
    TypeInfo *tinfo;

    int i_oid;
    int i_typowner;
    int i_typname;
    int i_typlen;
    int i_typprtlen;
    int i_typinput;
    int i_typoutput;
    int i_typreceive;
    int i_typsend;
    int i_typelem;
    int i_typdelim;
    int i_typdefault;
    int i_typrelid;
    int i_typbyval;

    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);
    
   /* find all base types */
   /* we include even the built-in types 
      because those may be used as array elements by user-defined types */
   /* we filter out the built-in types when 
      we dump out the types */

    sprintf(query, "SELECT oid, typowner,typname, typlen, typprtlen, "
            "typinput, typoutput, typreceive, typsend, typelem, typdelim, "
            "typdefault, typrelid,typbyval from pg_type");
    
    res = PQexec(g_conn,query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getTypes(): SELECT failed\n");
        exit_nicely(g_conn);
    }
    
    ntups = PQntuples(res);
    
    tinfo = (TypeInfo*)malloc(ntups * sizeof(TypeInfo));

    i_oid = PQfnumber(res,"oid");
    i_typowner = PQfnumber(res,"typowner");
    i_typname = PQfnumber(res,"typname");
    i_typlen = PQfnumber(res,"typlen");
    i_typprtlen = PQfnumber(res,"typprtlen");
    i_typinput = PQfnumber(res,"typinput");
    i_typoutput = PQfnumber(res,"typoutput");
    i_typreceive = PQfnumber(res,"typreceive");
    i_typsend = PQfnumber(res,"typsend");
    i_typelem = PQfnumber(res,"typelem");
    i_typdelim = PQfnumber(res,"typdelim");
    i_typdefault = PQfnumber(res,"typdefault");
    i_typrelid = PQfnumber(res,"typrelid");
    i_typbyval = PQfnumber(res,"typbyval");

    for (i=0;i<ntups;i++) {
        tinfo[i].oid = strdup(PQgetvalue(res,i,i_oid));
        tinfo[i].typowner = strdup(PQgetvalue(res,i,i_typowner));
        tinfo[i].typname = strdup(PQgetvalue(res,i,i_typname));
        tinfo[i].typlen = strdup(PQgetvalue(res,i,i_typlen));
        tinfo[i].typprtlen = strdup(PQgetvalue(res,i,i_typprtlen));
        tinfo[i].typinput = strdup(PQgetvalue(res,i,i_typinput));
        tinfo[i].typoutput = strdup(PQgetvalue(res,i,i_typoutput));
        tinfo[i].typreceive = strdup(PQgetvalue(res,i,i_typreceive));
        tinfo[i].typsend = strdup(PQgetvalue(res,i,i_typsend));
        tinfo[i].typelem = strdup(PQgetvalue(res,i,i_typelem));
        tinfo[i].typdelim = strdup(PQgetvalue(res,i,i_typdelim));
        tinfo[i].typdefault = strdup(PQgetvalue(res,i,i_typdefault));
        tinfo[i].typrelid = strdup(PQgetvalue(res,i,i_typrelid));

        if (strcmp(PQgetvalue(res,i,i_typbyval), "f") == 0)
            tinfo[i].passedbyvalue = 0;
        else
            tinfo[i].passedbyvalue = 1;

        /* check for user-defined array types,
           omit system generated ones */
        if ( (strcmp(tinfo[i].typelem, "0") != 0)  &&
             tinfo[i].typname[0] != '_')
            tinfo[i].isArray = 1;
        else
            tinfo[i].isArray = 0;
    }

    *numTypes = ntups;

    PQclear(res);

    res = PQexec(g_conn,"end");
    PQclear(res);

    return tinfo;
}

/*
 * getOperators:
 *    read all operators in the system catalogs and return them in the 
 * OprInfo* structure
 *
 *  numOprs is set to the number of operators read in 
 *    
 *
 */
OprInfo*
getOperators(int *numOprs)
{
    PGresult *res;
    int ntups;
    int i;
    char query[MAXQUERYLEN];

    OprInfo* oprinfo;

    int i_oid;
    int i_oprname;
    int i_oprkind;
    int i_oprcode;
    int i_oprleft;
    int i_oprright;
    int i_oprcom;
    int i_oprnegate;
    int i_oprrest;
    int i_oprjoin;
    int i_oprcanhash;
    int i_oprlsortop;
    int i_oprrsortop;
    
    /* find all operators, including builtin operators,
       filter out system-defined operators at dump-out time */
    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query, "SELECT oid, oprname, oprkind, oprcode, oprleft, "
            "oprright, oprcom, oprnegate, oprrest, oprjoin, oprcanhash, "
            "oprlsortop, oprrsortop from pg_operator");

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getOperators(): SELECT failed\n");
        exit_nicely(g_conn);
    }
    
    ntups = PQntuples(res);
    *numOprs = ntups;

    oprinfo = (OprInfo*)malloc(ntups * sizeof(OprInfo));

    i_oid = PQfnumber(res,"oid");
    i_oprname = PQfnumber(res,"oprname");
    i_oprkind = PQfnumber(res,"oprkind");
    i_oprcode = PQfnumber(res,"oprcode");
    i_oprleft = PQfnumber(res,"oprleft");
    i_oprright = PQfnumber(res,"oprright");
    i_oprcom = PQfnumber(res,"oprcom");
    i_oprnegate = PQfnumber(res,"oprnegate");
    i_oprrest = PQfnumber(res,"oprrest");
    i_oprjoin = PQfnumber(res,"oprjoin");
    i_oprcanhash = PQfnumber(res,"oprcanhash");
    i_oprlsortop = PQfnumber(res,"oprlsortop");
    i_oprrsortop = PQfnumber(res,"oprrsortop");

    for (i=0;i<ntups;i++) {
        oprinfo[i].oid = strdup(PQgetvalue(res,i,i_oid));
        oprinfo[i].oprname = strdup(PQgetvalue(res,i,i_oprname));
        oprinfo[i].oprkind = strdup(PQgetvalue(res,i,i_oprkind));
        oprinfo[i].oprcode = strdup(PQgetvalue(res,i,i_oprcode));
        oprinfo[i].oprleft = strdup(PQgetvalue(res,i,i_oprleft));
        oprinfo[i].oprright = strdup(PQgetvalue(res,i,i_oprright));
        oprinfo[i].oprcom = strdup(PQgetvalue(res,i,i_oprcom));
        oprinfo[i].oprnegate = strdup(PQgetvalue(res,i,i_oprnegate));
        oprinfo[i].oprrest = strdup(PQgetvalue(res,i,i_oprrest));
        oprinfo[i].oprjoin = strdup(PQgetvalue(res,i,i_oprjoin));
        oprinfo[i].oprcanhash = strdup(PQgetvalue(res,i,i_oprcanhash));
        oprinfo[i].oprlsortop = strdup(PQgetvalue(res,i,i_oprlsortop));
        oprinfo[i].oprrsortop = strdup(PQgetvalue(res,i,i_oprrsortop));
    }

    PQclear(res);
    res = PQexec(g_conn, "end");
    PQclear(res);

    return oprinfo;
}

void
clearTypeInfo(TypeInfo *tp, int numTypes)
{
int i;
for(i=0;i<numTypes;++i) {
    if(tp[i].oid) free(tp[i].oid);
    if(tp[i].typowner) free(tp[i].typowner);
    if(tp[i].typname) free(tp[i].typname);
    if(tp[i].typlen) free(tp[i].typlen);
    if(tp[i].typprtlen) free(tp[i].typprtlen);
    if(tp[i].typinput) free(tp[i].typinput);
    if(tp[i].typoutput) free(tp[i].typoutput);
    if(tp[i].typreceive) free(tp[i].typreceive);
    if(tp[i].typsend) free(tp[i].typsend);
    if(tp[i].typelem) free(tp[i].typelem);
    if(tp[i].typdelim) free(tp[i].typdelim);
    if(tp[i].typdefault) free(tp[i].typdefault);
    if(tp[i].typrelid) free(tp[i].typrelid);
	}
free(tp);
}

void
clearFuncInfo (FuncInfo *fun, int numFuncs)
{
int i,a;
if(!fun) return;
for(i=0;i<numFuncs;++i) {
    if(fun[i].oid) free(fun[i].oid);
    if(fun[i].proname) free(fun[i].proname);
    if(fun[i].proowner) free(fun[i].proowner);
    for(a=0;a<8;++a)
    if(fun[i].argtypes[a]) free(fun[i].argtypes[a]);
    if(fun[i].prorettype) free(fun[i].prorettype);
    if(fun[i].prosrc) free(fun[i].prosrc);
    if(fun[i].probin) free(fun[i].probin);
	}
free(fun);
}

void
clearTableInfo(TableInfo *tblinfo, int numTables)
{
int i,j;
for(i=0;i<numTables;++i) {
        if(tblinfo[i].oid) free (tblinfo[i].oid);
        if(tblinfo[i].relname) free (tblinfo[i].relname);
        if(tblinfo[i].relarch) free (tblinfo[i].relarch);
        if(tblinfo[i].relacl) free (tblinfo[i].relacl);
	for (j=0;j<tblinfo[i].numatts;j++) {
            if(tblinfo[i].attnames[j]) free (tblinfo[i].attnames[j]);
            if(tblinfo[i].typnames[j]) free (tblinfo[i].typnames[j]);
	    }
    if(tblinfo[i].attlen) free((int *)tblinfo[i].attlen);
    if(tblinfo[i].inhAttrs) free((int *)tblinfo[i].inhAttrs);
    if(tblinfo[i].attnames) free (tblinfo[i].attnames);
    if(tblinfo[i].typnames) free (tblinfo[i].typnames);
     }
free(tblinfo);
}

void 
clearInhInfo (InhInfo *inh, int numInherits) {
int i;
if(!inh) return;
for(i=0;i<numInherits;++i) {
    if(inh[i].oid) free(inh[i].oid);
    if(inh[i].inhrel) free(inh[i].inhrel);
    if(inh[i].inhparent) free(inh[i].inhparent);}
free(inh);
}

void
clearOprInfo(OprInfo *opr, int numOprs){
int i;
if(!opr) return;
for(i=0;i<numOprs;++i) {
     if(opr[i].oid) free(opr[i].oid);
     if(opr[i].oprname) free(opr[i].oprname);
     if(opr[i].oprkind) free(opr[i].oprkind);
     if(opr[i].oprcode) free(opr[i].oprcode);
     if(opr[i].oprleft) free(opr[i].oprleft);
     if(opr[i].oprright) free(opr[i].oprright);
     if(opr[i].oprcom) free(opr[i].oprcom);
     if(opr[i].oprnegate) free(opr[i].oprnegate);
     if(opr[i].oprrest) free(opr[i].oprrest);
     if(opr[i].oprjoin) free(opr[i].oprjoin);
     if(opr[i].oprcanhash) free(opr[i].oprcanhash);
     if(opr[i].oprlsortop) free(opr[i].oprlsortop);
     if(opr[i].oprrsortop) free(opr[i].oprrsortop);
	}
free(opr);
}

void
clearIndInfo(IndInfo *ind, int numIndices)
{
int i,a;
if(!ind) return;
for(i=0;i<numIndices;++i) {
    if(ind[i].indexrelname) free(ind[i].indexrelname);
    if(ind[i].indrelname) free(ind[i].indrelname);
    if(ind[i].indamname) free(ind[i].indamname);
    if(ind[i].indproc) free(ind[i].indproc);
    if(ind[i].indisunique) free(ind[i].indisunique);
    for(a=0;a<INDEX_MAX_KEYS;++a) {
    if(ind[i].indkey[a]) free(ind[i].indkey[a]);
    if(ind[i].indclass[a]) free(ind[i].indclass[a]);}
    }
free(ind);
}

void
clearAggInfo(AggInfo *agginfo, int numArgs)
{
int i;
if(!agginfo) return;
for(i=0;i<numArgs;++i) {
    if(agginfo[i].oid) free (agginfo[i].oid);
    if(agginfo[i].aggname) free (agginfo[i].aggname);
    if(agginfo[i].aggtransfn1) free (agginfo[i].aggtransfn1);
    if(agginfo[i].aggtransfn2) free (agginfo[i].aggtransfn2);
    if(agginfo[i].aggfinalfn) free (agginfo[i].aggfinalfn);
    if(agginfo[i].aggtranstype1) free (agginfo[i].aggtranstype1);
    if(agginfo[i].aggbasetype) free (agginfo[i].aggbasetype);
    if(agginfo[i].aggtranstype2) free (agginfo[i].aggtranstype2);
    if(agginfo[i].agginitval1) free (agginfo[i].agginitval1);
    if(agginfo[i].agginitval2) free (agginfo[i].agginitval2);
	}
free (agginfo);
}

/*
 * getAggregates:
 *    read all the user-defined aggregates in the system catalogs and
 * return them in the AggInfo* structure
 *
 * numAggs is set to the number of aggregates read in 
 *    
 *
 */
AggInfo*
getAggregates(int *numAggs)
{
    PGresult* res;
    int ntups;
    int i;
    char query[MAXQUERYLEN];
    AggInfo *agginfo;

    int i_oid;
    int i_aggname;
    int i_aggtransfn1;
    int i_aggtransfn2;
    int i_aggfinalfn;
    int i_aggtranstype1;
    int i_aggbasetype;
    int i_aggtranstype2;
    int i_agginitval1;
    int i_agginitval2;

    /* find all user-defined aggregates */

    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query, 
            "SELECT oid, aggname, aggtransfn1, aggtransfn2, aggfinalfn, "
            "aggtranstype1, aggbasetype, aggtranstype2, agginitval1, "
            "agginitval2 from pg_aggregate;");

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getAggregates(): SELECT failed\n");
        exit_nicely(g_conn);
    }

    ntups = PQntuples(res);
    *numAggs = ntups;

    agginfo = (AggInfo*)malloc(ntups * sizeof(AggInfo));

    i_oid = PQfnumber(res,"oid");
    i_aggname = PQfnumber(res,"aggname");
    i_aggtransfn1 = PQfnumber(res,"aggtransfn1");
    i_aggtransfn2 = PQfnumber(res,"aggtransfn2");
    i_aggfinalfn = PQfnumber(res,"aggfinalfn");
    i_aggtranstype1 = PQfnumber(res,"aggtranstype1");
    i_aggbasetype = PQfnumber(res,"aggbasetype");
    i_aggtranstype2 = PQfnumber(res,"aggtranstype2");
    i_agginitval1 = PQfnumber(res,"agginitval1");
    i_agginitval2 = PQfnumber(res,"agginitval2");

    for (i=0;i<ntups;i++) {
        agginfo[i].oid = strdup(PQgetvalue(res,i,i_oid));
        agginfo[i].aggname = strdup(PQgetvalue(res,i,i_aggname));
        agginfo[i].aggtransfn1 = strdup(PQgetvalue(res,i,i_aggtransfn1));
        agginfo[i].aggtransfn2 = strdup(PQgetvalue(res,i,i_aggtransfn2));
        agginfo[i].aggfinalfn = strdup(PQgetvalue(res,i,i_aggfinalfn));
        agginfo[i].aggtranstype1 = strdup(PQgetvalue(res,i,i_aggtranstype1));
        agginfo[i].aggbasetype = strdup(PQgetvalue(res,i,i_aggbasetype));
        agginfo[i].aggtranstype2 = strdup(PQgetvalue(res,i,i_aggtranstype2));
        agginfo[i].agginitval1 = strdup(PQgetvalue(res,i,i_agginitval1));
        agginfo[i].agginitval2 = strdup(PQgetvalue(res,i,i_agginitval2));
    }

    PQclear(res);

    res = PQexec(g_conn, "end");
    PQclear(res);
    return agginfo;
}

/*
 * getFuncs:
 *    read all the user-defined functions in the system catalogs and
 * return them in the FuncInfo* structure
 *
 * numFuncs is set to the number of functions read in 
 *    
 *
 */
FuncInfo*
getFuncs(int *numFuncs)
{
    PGresult *res;
    int ntups;
    int i;
    char query[MAXQUERYLEN];
    FuncInfo *finfo;

    int i_oid;
    int i_proname;
    int i_proowner;
    int i_prolang;
    int i_pronargs;
    int i_proargtypes;
    int i_prorettype;
    int i_proretset;
    int i_prosrc;
    int i_probin;

   /* find all user-defined funcs */

    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query, 
            "SELECT oid, proname, proowner, prolang, pronargs, prorettype, "
            "proretset, proargtypes, prosrc, probin from pg_proc "
            "where oid > '%d'::oid", 
            g_last_builtin_oid);

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getFuncs(): SELECT failed\n");
        exit_nicely(g_conn);
    }
    
    ntups = PQntuples(res);
    
    *numFuncs = ntups;

    finfo = (FuncInfo*)malloc(ntups * sizeof(FuncInfo));

    i_oid = PQfnumber(res,"oid");
    i_proname = PQfnumber(res,"proname");
    i_proowner = PQfnumber(res,"proowner");
    i_prolang = PQfnumber(res,"prolang");
    i_pronargs = PQfnumber(res,"pronargs");
    i_proargtypes = PQfnumber(res,"proargtypes");
    i_prorettype = PQfnumber(res,"prorettype");
    i_proretset = PQfnumber(res,"proretset");
    i_prosrc = PQfnumber(res,"prosrc");
    i_probin = PQfnumber(res,"probin");
    
    for (i=0;i<ntups;i++) {
        finfo[i].oid = strdup(PQgetvalue(res,i,i_oid));
        finfo[i].proname = strdup(PQgetvalue(res,i,i_proname));
        finfo[i].proowner = strdup(PQgetvalue(res,i,i_proowner));

        finfo[i].prosrc = checkForQuote(PQgetvalue(res,i,i_prosrc));
        finfo[i].probin = strdup(PQgetvalue(res,i,i_probin));

        finfo[i].prorettype = strdup(PQgetvalue(res,i,i_prorettype));
        finfo[i].retset = (strcmp(PQgetvalue(res,i,i_proretset),"t") == 0);
        finfo[i].nargs = atoi(PQgetvalue(res,i,i_pronargs));
        finfo[i].lang = (atoi(PQgetvalue(res,i,i_prolang)) == C_PROLANG_OID);

        parseArgTypes(finfo[i].argtypes, PQgetvalue(res,i,i_proargtypes));

        finfo[i].dumped = 0;
    }

    PQclear(res);
    res = PQexec(g_conn, "end");
    PQclear(res);

    return finfo;

}

/*
 * getTables
 *    read all the user-defined tables (no indices, no catalogs)
 * in the system catalogs return them in the TableInfo* structure
 *
 * numTables is set to the number of tables read in 
 *    
 *
 */
TableInfo*
getTables(int *numTables)
{
    PGresult *res;
    int ntups;
    int i;
    char query[MAXQUERYLEN];
    TableInfo *tblinfo;
    
    int i_oid;
    int i_relname;
    int i_relarch;
    int i_relkind;
    int i_relacl;

    /* find all the user-defined tables (no indices and no catalogs),
     ordering by oid is important so that we always process the parent
     tables before the child tables when traversing the tblinfo* 

      we ignore tables that start with xinv */

    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query, 
            "SELECT oid, relname, relarch, relkind, relacl from pg_class "
            "where (relkind = 'r' or relkind = 'S') and relname !~ '^pg_' "
            "and relname !~ '^xinv' order by oid;");

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getTables(): SELECT failed\n");
        exit_nicely(g_conn);
    }

    ntups = PQntuples(res);

    *numTables = ntups;

    tblinfo = (TableInfo*)malloc(ntups * sizeof(TableInfo));

    i_oid = PQfnumber(res,"oid");
    i_relname = PQfnumber(res,"relname");
    i_relarch = PQfnumber(res,"relarch");
    i_relkind = PQfnumber(res,"relkind");
    i_relacl = PQfnumber(res,"relacl");

    for (i=0;i<ntups;i++) {
        tblinfo[i].oid = strdup(PQgetvalue(res,i,i_oid));
        tblinfo[i].relname = strdup(PQgetvalue(res,i,i_relname));
        tblinfo[i].relarch = strdup(PQgetvalue(res,i,i_relarch));
        tblinfo[i].relacl = strdup(PQgetvalue(res,i,i_relacl));
        tblinfo[i].sequence = (strcmp (PQgetvalue(res,i,i_relkind), "S") == 0);
    }

    PQclear(res);
    res = PQexec(g_conn, "end");
    PQclear(res);

    return tblinfo;

}

/*
 * getInherits
 *    read all the inheritance information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numInherits is set to the number of tables read in 
 *    
 *
 */
InhInfo*
getInherits(int *numInherits)
{
    PGresult *res;
    int ntups;
    int i;
    char query[MAXQUERYLEN];
    InhInfo *inhinfo;
    
    int i_inhrel;
    int i_inhparent;

    /* find all the inheritance information */
    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query, "SELECT inhrel, inhparent from pg_inherits");

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getInherits(): SELECT failed\n");
        exit_nicely(g_conn);
    }
    
    ntups = PQntuples(res);

    *numInherits = ntups;

    inhinfo = (InhInfo*)malloc(ntups * sizeof(InhInfo));

    i_inhrel = PQfnumber(res,"inhrel");
    i_inhparent = PQfnumber(res,"inhparent");

    for (i=0;i<ntups;i++) {
        inhinfo[i].inhrel = strdup(PQgetvalue(res,i,i_inhrel));
        inhinfo[i].inhparent = strdup(PQgetvalue(res,i,i_inhparent));
    }

    PQclear(res);
    res = PQexec(g_conn, "end");
    PQclear(res);
    return inhinfo;
}

/*
 * getTableAttrs -
 *    for each table in tblinfo, read its attributes types and names
 * 
 * this is implemented in a very inefficient way right now, looping
 * through the tblinfo and doing a join per table to find the attrs and their 
 * types
 *
 *  modifies tblinfo
 */
void
getTableAttrs(TableInfo* tblinfo, int numTables)
{
    int i,j;
    char q[MAXQUERYLEN];
    int i_attname;
    int i_typname;
    int i_attlen;
    PGresult *res;
    int ntups;

    for (i=0;i<numTables;i++)  {

        /* skip archive tables */
        if (isArchiveName(tblinfo[i].relname))
            continue;
        
        if ( tblinfo[i].sequence )
            continue;

        /* find all the user attributes and their types*/
        /* we must read the attribute names in attribute number order! */
        /* because we will use the attnum to index into the attnames array 
           later */
        if (g_verbose) 
            fprintf(stderr,"%s finding the attrs and types for table: %s %s\n",
                g_comment_start,
                tblinfo[i].relname,
                g_comment_end);

        sprintf(q,"SELECT a.attnum, a.attname, t.typname, a.attlen "
                "from pg_attribute a, pg_type t "
                "where a.attrelid = '%s'::oid and a.atttypid = t.oid "
                "and a.attnum > 0 order by attnum",
                tblinfo[i].oid);
        res = PQexec(g_conn, q);
        if (!res || 
            PQresultStatus(res) != PGRES_TUPLES_OK) {
            fprintf(stderr,"getTableAttrs(): SELECT failed\n");
            exit_nicely(g_conn);
        }
        
        ntups = PQntuples(res);

        i_attname = PQfnumber(res,"attname");
        i_typname = PQfnumber(res,"typname");
        i_attlen  = PQfnumber(res,"attlen");

        tblinfo[i].numatts = ntups;
        tblinfo[i].attnames = (char**) malloc( ntups * sizeof(char*));
        tblinfo[i].typnames = (char**) malloc( ntups * sizeof(char*));
        tblinfo[i].attlen   = (int*) malloc(ntups * sizeof(int));
        tblinfo[i].inhAttrs = (int*) malloc (ntups * sizeof(int));
        tblinfo[i].parentRels = NULL;
        tblinfo[i].numParents = 0;
        for (j=0;j<ntups;j++) {
            tblinfo[i].attnames[j] = strdup(PQgetvalue(res,j,i_attname));
            tblinfo[i].typnames[j] = strdup(PQgetvalue(res,j,i_typname));
            tblinfo[i].attlen[j] = atoi(PQgetvalue(res,j,i_attlen));
            if (tblinfo[i].attlen[j] > 0) 
              tblinfo[i].attlen[j] = tblinfo[i].attlen[j] - 4;
            tblinfo[i].inhAttrs[j] = 0; /* this flag is set in flagInhAttrs()*/
        }
        PQclear(res);
    } 
}


/*
 * getIndices
 *    read all the user-defined indices information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numIndices is set to the number of indices read in 
 *    
 *
 */
IndInfo*
getIndices(int *numIndices)
{
    int i;
    char query[MAXQUERYLEN];
    PGresult *res;
    int ntups;
    IndInfo *indinfo;

    int i_indexrelname;
    int i_indrelname;
    int i_indamname;
    int i_indproc;
    int i_indkey;
    int i_indclass;
    int i_indisunique;
    
    /* 
       find all the user-defined indices.
       We do not handle partial indices.

       skip 'Xinx*' - indices on inversion objects

       this is a 4-way join !!
    */
       
    res = PQexec(g_conn, "begin");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"BEGIN command failed\n");
        exit_nicely(g_conn);
    }
    PQclear(res);

    sprintf(query,
            "SELECT t1.relname as indexrelname, t2.relname as indrelname, "
            "i.indproc, i.indkey, i.indclass, "
            "a.amname as indamname, i.indisunique "
            "from pg_index i, pg_class t1, pg_class t2, pg_am a "
            "where t1.oid = i.indexrelid and t2.oid = i.indrelid "
            "and t1.relam = a.oid and i.indexrelid > '%d'::oid "
            "and t2.relname !~ '^pg_' and t1.relname !~ '^Xinx' ;",
            g_last_builtin_oid);

    res = PQexec(g_conn, query);
    if (!res || 
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"getIndices(): SELECT failed\n");
        exit_nicely(g_conn);
    }

    ntups = PQntuples(res);

    *numIndices = ntups;

    indinfo = (IndInfo*)malloc(ntups * sizeof (IndInfo));

    i_indexrelname = PQfnumber(res,"indexrelname");
    i_indrelname = PQfnumber(res,"indrelname");
    i_indamname = PQfnumber(res,"indamname");
    i_indproc = PQfnumber(res,"indproc");
    i_indkey = PQfnumber(res,"indkey");
    i_indclass = PQfnumber(res,"indclass");
    i_indisunique = PQfnumber(res,"indisunique");

    for (i=0;i<ntups;i++) {
        indinfo[i].indexrelname = strdup(PQgetvalue(res,i,i_indexrelname));
        indinfo[i].indrelname = strdup(PQgetvalue(res,i,i_indrelname));
        indinfo[i].indamname = strdup(PQgetvalue(res,i,i_indamname));
        indinfo[i].indproc = strdup(PQgetvalue(res,i,i_indproc));
        parseArgTypes ((char **)indinfo[i].indkey, 
        	(const char*)PQgetvalue(res,i,i_indkey));
        parseArgTypes ((char **)indinfo[i].indclass,
        	(const char*)PQgetvalue(res,i,i_indclass));
        indinfo[i].indisunique = strdup(PQgetvalue(res,i,i_indisunique));
    }
    PQclear(res);
    res = PQexec(g_conn,"end");
    if(res) PQclear(res);
    return indinfo;
}

/*
 * dumpTypes
 *    writes out to fout the queries to recreate all the user-defined types
 *
 */
void
dumpTypes(FILE* fout, FuncInfo* finfo, int numFuncs,
          TypeInfo* tinfo, int numTypes)
{
    int i;
    char q[MAXQUERYLEN];
    int funcInd;

    for (i=0;i<numTypes;i++) {

        /* skip all the builtin types */
        if (atoi(tinfo[i].oid) < g_last_builtin_oid)
            continue;

        /* skip relation types */
        if (atoi(tinfo[i].typrelid) != 0)
            continue;

        /* skip all array types that start w/ underscore */
        if ( (tinfo[i].typname[0] == '_') &&
             (strcmp(tinfo[i].typinput, "array_in") == 0))
            continue;

        /* before we create a type, we need to create the input and
           output functions for it, if they haven't been created already */
        funcInd = findFuncByName(finfo, numFuncs, tinfo[i].typinput);
        if (funcInd !=  -1) 
            dumpOneFunc(fout,finfo,funcInd,tinfo,numTypes);

        funcInd = findFuncByName(finfo, numFuncs, tinfo[i].typoutput);
        if (funcInd !=  -1) 
            dumpOneFunc(fout,finfo,funcInd,tinfo,numTypes);

        sprintf(q,
                "CREATE TYPE %s "
                "( internallength = %s, externallength = %s, input = %s, "
                "output = %s, send = %s, receive = %s, default = '%s'",
                tinfo[i].typname,
                tinfo[i].typlen,
                tinfo[i].typprtlen,
                tinfo[i].typinput,
                tinfo[i].typoutput,
                tinfo[i].typsend,
                tinfo[i].typreceive,
                tinfo[i].typdefault);

        if (tinfo[i].isArray) {
            char* elemType;

            elemType = findTypeByOid(tinfo, numTypes, tinfo[i].typelem);
            
            sprintf(q,"%s, element = %s, delimiter = '%s'",
                    q, elemType,tinfo[i].typdelim);
        }
        if (tinfo[i].passedbyvalue)
            strcat(q,",passedbyvalue);\n");
        else
            strcat(q,");\n");
            
        fputs(q,fout);
    }
}

/*
 * dumpFuncs
 *    writes out to fout the queries to recreate all the user-defined functions
 *
 */
void
dumpFuncs(FILE* fout, FuncInfo* finfo, int numFuncs, 
          TypeInfo *tinfo, int numTypes)
{
    int i;
    for (i=0;i<numFuncs;i++)  {
        dumpOneFunc(fout,finfo,i,tinfo,numTypes);
    }
}

/*
 * dumpOneFunc:
 *    dump out only one function,  the index of which is given in the third
 *  argument
 *
 */

void
dumpOneFunc(FILE* fout, FuncInfo* finfo, int i,
            TypeInfo *tinfo, int numTypes)
{
    char q[MAXQUERYLEN];
    int j;
    
    if (finfo[i].dumped)
        return;
    else
        finfo[i].dumped = 1;

    sprintf(q,"CREATE FUNCTION %s (",finfo[i].proname);
    for (j=0;j<finfo[i].nargs;j++) {
        char* typname;
        typname = findTypeByOid(tinfo, numTypes, finfo[i].argtypes[j]);
        sprintf(q, "%s%s%s",
                q,  
                (j > 0) ? "," : "",
                typname);
    }
    sprintf(q,"%s ) RETURNS %s%s AS '%s' LANGUAGE '%s';\n",
                q, 
            finfo[i].retset ? " SETOF " : "",
            findTypeByOid(tinfo, numTypes, finfo[i].prorettype),
            (finfo[i].lang) ? finfo[i].probin : finfo[i].prosrc,
            (finfo[i].lang) ? "C" : "SQL");
    
    fputs(q,fout);

}

/*
 * dumpOprs
 *    writes out to fout the queries to recreate all the user-defined operators
 *
 */
void 
dumpOprs(FILE* fout, OprInfo* oprinfo, int numOperators,
         TypeInfo *tinfo, int numTypes)
{
    int i;
    char q[MAXQUERYLEN];
    char leftarg[MAXQUERYLEN];
    char rightarg[MAXQUERYLEN];
    char commutator[MAXQUERYLEN];
    char negator[MAXQUERYLEN];
    char restrict[MAXQUERYLEN];
    char join[MAXQUERYLEN];
    char sortop[MAXQUERYLEN];

    for (i=0;i<numOperators;i++) {

        /* skip all the builtin oids */
        if (atoi(oprinfo[i].oid) < g_last_builtin_oid)
            continue;

        /* some operator are invalid because they were the result
           of user defining operators before commutators exist */
        if (strcmp(oprinfo[i].oprcode, "-") == 0)
            continue;

        leftarg[0] = '\0';
        rightarg[0] = '\0';
        /* right unary means there's a left arg
           and left unary means there's a right arg */
        if (strcmp(oprinfo[i].oprkind, "r") == 0 || 
            strcmp(oprinfo[i].oprkind, "b") == 0 ) {
            sprintf(leftarg, ", LEFTARG = %s ",
                    findTypeByOid(tinfo, numTypes, oprinfo[i].oprleft));
        } 
        if (strcmp(oprinfo[i].oprkind, "l") == 0 || 
            strcmp(oprinfo[i].oprkind, "b") == 0 ) {
            sprintf(rightarg, ", RIGHTARG = %s ",
                    findTypeByOid(tinfo, numTypes, oprinfo[i].oprright));
        }
        if (strcmp(oprinfo[i].oprcom, "0") == 0) 
            commutator[0] = '\0';
        else
            sprintf(commutator,", COMMUTATOR = %s ",
                    findOprByOid(oprinfo, numOperators, oprinfo[i].oprcom));

        if (strcmp(oprinfo[i].oprnegate, "0") == 0) 
            negator[0] = '\0';
        else
            sprintf(negator,", NEGATOR = %s ",
                    findOprByOid(oprinfo, numOperators, oprinfo[i].oprnegate));

        if (strcmp(oprinfo[i].oprrest, "-") == 0)
            restrict[0] = '\0';
        else
            sprintf(restrict,", RESTRICT = %s ", oprinfo[i].oprrest);
                    
        if (strcmp(oprinfo[i].oprjoin,"-") == 0)
            join[0] = '\0';
        else
            sprintf(join,", JOIN = %s ", oprinfo[i].oprjoin);
                    
        if (strcmp(oprinfo[i].oprlsortop, "0") == 0) 
            sortop[0] = '\0';
        else
            {
            sprintf(sortop,", SORT = %s ",
                    findOprByOid(oprinfo, numOperators,
                                 oprinfo[i].oprlsortop));
            if (strcmp(oprinfo[i].oprrsortop, "0") != 0)
                sprintf(sortop, "%s , %s", sortop, 
                        findOprByOid(oprinfo, numOperators,
                                     oprinfo[i].oprlsortop));
        }

        sprintf(q,
                "CREATE OPERATOR %s "
                "(PROCEDURE = %s %s %s %s %s %s %s %s %s);\n ",
                oprinfo[i].oprname,
                oprinfo[i].oprcode,
                leftarg,
                rightarg,
                commutator,
                negator,
                restrict,
                (strcmp(oprinfo[i].oprcanhash, "t")) ? ", HASHES" : "",
                join,
                sortop);

        fputs(q,fout);
    }
}

/*
 * dumpAggs
 *    writes out to fout the queries to create all the user-defined aggregates
 *
 */
void
dumpAggs(FILE* fout, AggInfo* agginfo, int numAggs,
        TypeInfo *tinfo, int numTypes)
{
    int i;
    char q[MAXQUERYLEN];
    char sfunc1[MAXQUERYLEN];
    char sfunc2[MAXQUERYLEN];
    char finalfunc[MAXQUERYLEN];
    char comma1[2], comma2[2];

    for (i=0;i<numAggs;i++) {
        /* skip all the builtin oids */
        if (atoi(agginfo[i].oid) < g_last_builtin_oid)
            continue;

        if ( strcmp(agginfo[i].aggtransfn1, "-") == 0) 
            sfunc1[0] = '\0';
        else {
            sprintf(sfunc1, 
                    "SFUNC1 = %s, BASETYPE = %s, STYPE1 = %s",
                    agginfo[i].aggtransfn1,
                    findTypeByOid(tinfo,numTypes,agginfo[i].aggbasetype),
                    findTypeByOid(tinfo,numTypes,agginfo[i].aggtranstype1));
            if (agginfo[i].agginitval1)
                sprintf(sfunc1, "%s ,INITCOND1 = '%s'",
                        sfunc1, agginfo[i].agginitval1);
            
        }

        if ( strcmp(agginfo[i].aggtransfn2, "-") == 0) 
            sfunc2[0] = '\0';
        else {
            sprintf(sfunc2, 
                    "SFUNC2 = %s, STYPE2 = %s",
                    agginfo[i].aggtransfn2,
                    findTypeByOid(tinfo,numTypes,agginfo[i].aggtranstype2));
            if (agginfo[i].agginitval2)
                sprintf(sfunc2,"%s ,INITCOND2 = '%s'",
                        sfunc2, agginfo[i].agginitval2);
        }
        
        if ( strcmp(agginfo[i].aggfinalfn, "-") == 0)
            finalfunc[0] = '\0';
        else {
            sprintf(finalfunc, "FINALFUNC = %s", agginfo[i].aggfinalfn);
        }
        if (sfunc1[0] != '\0' && sfunc2[0] != '\0') {
            comma1[0] = ','; comma1[1] = '\0';
        } else
            comma1[0] = '\0';

        if (finalfunc[0] != '\0' && (sfunc1[0] != '\0' || sfunc2[0] != '\0')) {
            comma2[0] = ',';comma2[1] = '\0';
        } else
            comma2[0] = '\0';

        sprintf(q,"CREATE AGGREGATE %s ( %s %s %s %s %s );\n",
                agginfo[i].aggname,
                sfunc1,
                comma1,
                sfunc2,
                comma2,
                finalfunc);

        fputs(q,fout);
    }
}

/*
 * dumpTables:
 *    write out to fout all the user-define tables
 */

void dumpTables(FILE* fout, TableInfo *tblinfo, int numTables,
           InhInfo *inhinfo, int numInherits,
           TypeInfo *tinfo, int numTypes, const char *tablename,
           const bool acls)
{
    int i,j,k;
    char q[MAXQUERYLEN];
    char **parentRels;  /* list of names of parent relations */
    int numParents;
    int actual_atts; /* number of attrs in this CREATE statment */
    const char *archiveMode;

    for (i=0;i<numTables;i++) {

        if (!tablename || (!strcmp(tblinfo[i].relname,tablename))) {

	    /* Skip VIEW relations */
            if (isViewRule(tblinfo[i].relname))
                continue;

            /* skip archive names*/
            if (isArchiveName(tblinfo[i].relname))
                continue;
            
            if ( tblinfo[i].sequence )
            {
            	dumpSequence (fout, tblinfo[i]);
            	continue;
            }

            parentRels = tblinfo[i].parentRels;
            numParents = tblinfo[i].numParents;

            sprintf(q, "CREATE TABLE %s (", tblinfo[i].relname);
            actual_atts = 0;
            for (j=0;j<tblinfo[i].numatts;j++) {
                if (tblinfo[i].inhAttrs[j] == 0) {
                
                    /* Show lengths on bpchar and varchar */
                    if (!strcmp(tblinfo[i].typnames[j],"bpchar")) {
                        sprintf(q, "%s%s%s char",
                                q,
                                (actual_atts > 0) ? ", " : "",
                                tblinfo[i].attnames[j]);

                        /* stored length can be -1 (variable) */
                        if (tblinfo[i].attlen[j] > 0)
                                sprintf(q, "%s(%d)",
                                        q,
                                        tblinfo[i].attlen[j]);
                        actual_atts++;
                    }
                    else if (!strcmp(tblinfo[i].typnames[j],"varchar")) {
                        sprintf(q, "%s%s%s %s",
                                q,
                                (actual_atts > 0) ? ", " : "",
                                tblinfo[i].attnames[j],
                                tblinfo[i].typnames[j]);

                        /* stored length can be -1 (variable) */
                        if (tblinfo[i].attlen[j] > 0)
                                sprintf(q, "%s(%d)",
                                        q,
                                        tblinfo[i].attlen[j]);
                        actual_atts++;
                    }
                    else {    
                        sprintf(q, "%s%s%s %s",
                                q,
                                (actual_atts > 0) ? ", " : "",
                                tblinfo[i].attnames[j],
                                tblinfo[i].typnames[j]);
                        actual_atts++;
                    }
                }
            }

            strcat(q,")");

            if (numParents > 0) {
                sprintf(q, "%s inherits ( ",q);
                for (k=0;k<numParents;k++){
                sprintf(q, "%s%s%s",
                            q,
                            (k>0) ? ", " : "",
                            parentRels[k]);
                }
                strcat(q,")");
            }

            switch(tblinfo[i].relarch[0]) {
            case 'n':
                archiveMode = "none";
                break;
            case 'h':
                archiveMode = "heavy";
                break;
            case 'l':
                archiveMode = "light";
                break;
            default:
                fprintf(stderr, "unknown archive mode\n");
                archiveMode = "none";
                break;
            }
            
            sprintf(q, "%s archive = %s;\n",
                    q,
                    archiveMode);
            fputs(q,fout);

            if(acls) 
              fprintf(fout, 
                      "UPDATE pg_class SET relacl='%s' where relname='%s';\n",
                      tblinfo[i].relacl, tblinfo[i].relname);
        }
    }
}

/*
 * dumpIndices:
 *    write out to fout all the user-define indices
 */
void 
dumpIndices(FILE* fout, IndInfo* indinfo, int numIndices,
            TableInfo* tblinfo, int numTables, const char *tablename)
{
    int i, k;
    int tableInd;
    char attlist[1000];
    char *classname[INDEX_MAX_KEYS];
    char *funcname; /* the name of the function to comput the index key from*/
    int indkey, indclass;
    int nclass;

    char q[MAXQUERYLEN];
    PGresult *res;

    for (i=0;i<numIndices;i++) {
        tableInd = findTableByName(tblinfo, numTables,
                                   indinfo[i].indrelname);

        if (strcmp(indinfo[i].indproc,"0") == 0) {
            funcname = NULL;
        } else {
            /* the funcname is an oid which we use to 
               find the name of the pg_proc.  We need to do this
               because getFuncs() only reads in the user-defined funcs
               not all the funcs.  We might not find what we want
               by looking in FuncInfo**/
            sprintf(q,
                    "SELECT proname from pg_proc "
                    "where pg_proc.oid = '%s'::oid",
                    indinfo[i].indproc);
            res = PQexec(g_conn, q);
    	    if ( !res || PQresultStatus(res) != PGRES_TUPLES_OK )
    	    {
        	fprintf(stderr,"dumpIndices(): SELECT (funcname) failed\n");
        	exit_nicely(g_conn);
    	    }
            funcname = strdup(PQgetvalue(res, 0,
                                         PQfnumber(res,"proname")));
            PQclear(res);
        }

	/* convert opclass oid(s) into names */
        for (nclass = 0; nclass < INDEX_MAX_KEYS; nclass++)
        {
            indclass = atoi(indinfo[i].indclass[nclass]);
            if ( indclass == 0 )
            	break;
            sprintf(q,
                    "SELECT opcname from pg_opclass "
                    "where pg_opclass.oid = '%u'::oid",
                    indclass);
            res = PQexec(g_conn, q);
    	    if ( !res || PQresultStatus(res) != PGRES_TUPLES_OK )
    	    {
        	fprintf(stderr,"dumpIndices(): SELECT (classname) failed\n");
        	exit_nicely(g_conn);
    	    }
            classname[nclass] = strdup(PQgetvalue(res, 0,
                                         PQfnumber(res,"opcname")));
            PQclear(res);
        }
        
        if ( funcname && nclass != 1 )
    	{
            fprintf(stderr,"dumpIndices(): Must be exactly one OpClass "
            	"for functional index %s\n", indinfo[i].indexrelname);
            exit_nicely(g_conn);
    	}

	/* convert attribute numbers into attribute list */
        for (k = 0, attlist[0] = 0; k < INDEX_MAX_KEYS; k++)
        {
            char * attname;
            
            indkey = atoi(indinfo[i].indkey[k]);
            if ( indkey == 0 )
            	break;
            indkey--; 
            if (indkey == ObjectIdAttributeNumber - 1)
            	attname = "oid";
            else
            	attname = tblinfo[tableInd].attnames[indkey];
            if ( funcname )
            	sprintf (attlist + strlen(attlist), "%s%s",
            		( k == 0 ) ? "" : ", ", attname);
            else
            {
        	if ( k >= nclass )
    		{
            	    fprintf(stderr,"dumpIndices(): OpClass not found for "
            		"attribute %s of index %s\n", 
            		attname, indinfo[i].indexrelname);
            	    exit_nicely(g_conn);
    		}
            	sprintf (attlist + strlen(attlist), "%s%s %s",
            		( k == 0 ) ? "" : ", ", attname, classname[k]);
            	free (classname[k]);
            }
        }
        
        if (!tablename || (!strcmp(indinfo[i].indrelname,tablename))) {
        
            sprintf(q,"CREATE %s INDEX %s on %s using %s (",
		    (strcmp(indinfo[i].indisunique, "t") == 0) ? "UNIQUE" : "",
                    indinfo[i].indexrelname,
                    indinfo[i].indrelname,
                    indinfo[i].indamname);
            if (funcname) {
                sprintf(q, "%s %s (%s) %s );\n",
                        q, funcname, attlist, classname[0]);
                free(funcname); 
                free(classname[0]); 
            } else
                sprintf(q, "%s %s );\n",
                        q, attlist);

            fputs(q,fout);
        }    
    }

}
/*
 * dumpTuples --
 *    prints out the tuples in ASCII representation. The output is a valid
 *    input to COPY FROM stdin.
 *
 *    We only need to do this for POSTGRES 4.2 databases since the
 *    COPY TO statment doesn't escape newlines properly. It's been fixed
 *    in Postgres95.
 * 
 * the attrmap passed in tells how to map the attributes copied in to the
 * attributes copied out
 */
void
dumpTuples(PGresult *res, FILE *fout, int* attrmap)
{
    int j, k;
    int m, n;
    char **outVals = NULL; /* values to copy out */

    n = PQntuples(res);
    m = PQnfields(res);
    
    if ( m > 0 ) {
        /*
         * Print out the tuples but only print tuples with at least
         * 1 field.
         */
        outVals = (char**)malloc(m * sizeof(char*));

        for (j = 0; j < n; j++) {
            for (k = 0; k < m; k++) {
                outVals[attrmap[k]] = PQgetvalue(res, j, k);
            }
            for (k = 0; k < m; k++) {
                char *pval = outVals[k];

                if (k!=0)
                    fputc('\t', fout);  /* delimiter for attribute */

                if (pval) {
                    while (*pval != '\0') {
                        /* escape tabs, newlines and backslashes */
                        if (*pval=='\t' || *pval=='\n' || *pval=='\\')
                            fputc('\\', fout);
                        fputc(*pval, fout);
                        pval++;
                    }
                }
            }
            fputc('\n', fout);  /* delimiter for a tuple */
        }
        free (outVals);
    }
}

/*
 * setMaxOid -
 * find the maximum oid and generate a COPY statement to set it
*/

void
setMaxOid(FILE *fout)
{
    PGresult *res;
    Oid max_oid;
    
    res = PQexec(g_conn, "CREATE TABLE pgdump_oid (dummy int4)");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"Can not create pgdump_oid table\n");
        exit_nicely(g_conn);
    }
    PQclear(res);
    res = PQexec(g_conn, "INSERT INTO pgdump_oid VALUES (0)");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"Can not insert into pgdump_oid table\n");
        exit_nicely(g_conn);
    }
    max_oid = atol(PQoidStatus(res));
    if (max_oid == 0) {
        fprintf(stderr,"Invalid max id in setMaxOid\n");
        exit_nicely(g_conn);
    }
    PQclear(res);
    res = PQexec(g_conn, "DROP TABLE pgdump_oid;");
    if (!res || 
        PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr,"Can not drop pgdump_oid table\n");
        exit_nicely(g_conn);
    }
    PQclear(res);
    if (g_verbose) 
        fprintf(stderr, "%s maximum system oid is %d %s\n",
            g_comment_start,  max_oid, g_comment_end);
    fprintf(fout, "CREATE TABLE pgdump_oid (dummy int4);\n");
    fprintf(fout, "COPY pgdump_oid WITH OIDS FROM stdin;\n");
    fprintf(fout, "%-d\t0\n", max_oid);
    fprintf(fout, "\\.\n");
    fprintf(fout, "DROP TABLE pgdump_oid;\n");
}       

/*
 * findLastBuiltInOid -
 * find the last built in oid 
 * we do this by looking up the oid of 'template1' in pg_database,
 * this is probably not foolproof but comes close 
*/

int
findLastBuiltinOid(void)
{
    PGresult* res;
    int ntups;
    int last_oid;

    res = PQexec(g_conn, 
                 "SELECT oid from pg_database where datname = 'template1';");
    if (res == NULL ||
        PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr,"pg_dump error in finding the template1 database\n");
        exit_nicely(g_conn);
    }
    ntups = PQntuples(res);
    if (ntups != 1) {
        fprintf(stderr,"pg_dump: couldn't find the template1 database.  "
                "You are really hosed.\nGiving up.\n");
        exit_nicely(g_conn);
    }
    last_oid = atoi(PQgetvalue(res, 0, PQfnumber(res, "oid")));
    PQclear(res);
    return last_oid;
}


/*
 * checkForQuote:
 *    checks a string for quote characters and quotes them
 */
char*
checkForQuote(const char* s)
{
    char *r;
    char c;
    char *result;

    int j = 0;

    r = malloc(strlen(s)*3 + 1);  /* definitely long enough */

    while ( (c = *s) != '\0') {

        if (c == '\'') {
            r[j++] = '\''; /* quote the single quotes */
        }
        r[j++] = c;
        s++;
    }
    r[j] = '\0';

    result = strdup(r);
    free(r);

    return result;
    
}


static void dumpSequence (FILE* fout, TableInfo tbinfo)
{
    PGresult *res;
    int4 last, incby, maxv, minv, cache;
    char cycled, called, *t;
    char query[MAXQUERYLEN];

    sprintf (query, 
            "SELECT sequence_name, last_value, increment_by, max_value, "
            "min_value, cache_value, is_cycled, is_called from %s;",
            tbinfo.relname);

    res = PQexec (g_conn, query);
    if ( !res || PQresultStatus(res) != PGRES_TUPLES_OK )
    {
        fprintf (stderr,"dumpSequence(%s): SELECT failed\n", tbinfo.relname);
        exit_nicely (g_conn);
    }

    if ( PQntuples (res) != 1 )
    {
        fprintf (stderr,"dumpSequence(%s): %d (!= 1) tuples returned by SELECT\n",
        	tbinfo.relname, PQntuples(res));
        exit_nicely (g_conn);
    }

    if ( strcmp (PQgetvalue (res,0,0), tbinfo.relname) != 0 )
    {
        fprintf (stderr, "dumpSequence(%s): different sequence name "
        		 "returned by SELECT: %s\n",
        	tbinfo.relname, PQgetvalue (res,0,0));
        exit_nicely (g_conn);
    }


    last = atoi (PQgetvalue (res,0,1));
    incby = atoi (PQgetvalue (res,0,2));
    maxv = atoi (PQgetvalue (res,0,3));
    minv = atoi (PQgetvalue (res,0,4));
    cache = atoi (PQgetvalue (res,0,5));
    t = PQgetvalue (res,0,6);
    cycled = *t;
    t = PQgetvalue (res,0,7);
    called = *t;

    PQclear (res);
    
    sprintf (query, 
            "CREATE SEQUENCE %s start %d increment %d maxvalue %d "
            "minvalue %d  cache %d %s;\n",
            tbinfo.relname, last, incby, maxv, minv, cache,
            (cycled == 't') ? "cycle" : "");

    fputs (query, fout);
    
    if ( called == 'f' )
    	return;			/* nothing to do more */

    sprintf (query, "SELECT nextval ('%s');\n", tbinfo.relname);
    fputs (query, fout);

}

