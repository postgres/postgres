/*-------------------------------------------------------------------------
 *
 * pg4_dump.c--
 *    pg4_dump is an utility for dumping out a postgres database
 * into a script file.
 *
 *  pg4_dump will read the system catalogs from a postgresV4r2 database and 
 *  dump out a script that reproduces the schema of the database in terms of
 *        user-defined types
 *        user-defined functions
 *        tables
 *        indices
 *        aggregates
 *        operators
 *
 * the output script is either POSTQUEL or SQL 
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    /usr/local/devel/pglite/cvs/src/bin/pg_dump/pg4_dump.c,v 1.1 1995/05/18 19:23:53 jolly Exp
 *
 *-------------------------------------------------------------------------
 */


#include <stdlib.h>
#include <stdio.h>
#include <sys/param.h>	/* for MAXHOSTNAMELEN on most */
#ifndef  MAXHOSTNAMELEN
#include <netdb.h>	/* for MAXHOSTNAMELEN on some */
#endif
#endif

#include "tmp/postgres.h"
#include "tmp/libpq-fe.h"
#include "libpq/auth.h"

#include "pg_dump.h"

extern char *optarg;
extern int optind, opterr;

/* these are used in libpq  */
extern char	*PQhost;     /* machine on which the backend is running */
extern char	*PQport;     /* comm. port with the postgres backend. */
extern char	*PQtty;      /* the tty where postgres msgs are displayed */
extern char	*PQdatabase; /* the postgres db to access.  */

/* global decls */
int g_verbose;  /* verbose flag */
int g_last_builtin_oid; /* value of the last builtin oid */
FILE *g_fout;     /* the script file */

char g_opaque_type[10]; /* name for the opaque type */

/* placeholders for the delimiters for comments */
char g_comment_start[10]; 
char g_comment_end[10]; 

int g_outputSQL; /* if 1, output SQL, otherwise , output Postquel */

static
usage(char* progname)
{
    fprintf(stderr, "usage:  %s [options] [dbname]\n",progname);
    fprintf(stderr, "\t -f filename \t\t script output filename\n");
    fprintf(stderr, "\t -H hostname \t\t server host name\n");
    fprintf(stderr, "\t -o [SQL|POSTQUEL} \t\t output format\n");
    fprintf(stderr, "\t -p port     \t\t server port number\n");
    fprintf(stderr, "\t -v          \t\t verbose\n");
    fprintf(stderr, "\t -S          \t\t dump out only the schema, no data\n");
    fprintf(stderr, "\n if dbname is not supplied, then the DATABASE environment name is used\n");
    fprintf(stderr, "\n");

    fprintf(stderr, "\tpg4_dump dumps out postgres databases and produces a script file\n");
    fprintf(stderr, "\tof query commands to regenerate the schema\n");
    fprintf(stderr, "\tThe output format is either POSTQUEL or SQL.  The default is SQL\n");
    exit(1);
}

void
main(int argc, char** argv)
{
    int c;
    char* progname;
    char* filename;
    char* dbname;
    char *username, usernamebuf[NAMEDATALEN + 1];
    char hostbuf[MAXHOSTNAMELEN];
    int schemaOnly;

    TableInfo *tblinfo;
    int numTables;


    dbname = NULL;
    filename = NULL;
    g_verbose = 0;
    g_outputSQL = 1;
    schemaOnly = 0;

    progname = *argv;

    while ((c = getopt(argc, argv,"f:H:o:p:vSD")) != EOF) {
	switch(c) {
	case 'f': /* output file name */
	    filename = optarg;
	    break;
	case 'H' : /* server host */
	    PQhost = optarg;
	    break;
	case 'o': 
	    {
	    char *lang = optarg;
	    if (lang) {
		if (strcmp(lang,"SQL") != 0)
		    g_outputSQL = 0;
	    }
	    }
	    break;
	case 'p' : /* server port */
	    PQport = optarg;
	    break;
	case 'v': /* verbose */
	    g_verbose = 1;
	    break;
	case 'S': /* dump schema only */
	    schemaOnly = 1;
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
	    fprintf(stderr,"%s: could not open output file named %s for writing\n",
		    progname, filename);
	    exit(2);
	}
    }

    /* Determine our username (according to the authentication system, if
     * there is one).
     */
    if ((username = fe_getauthname()) == (char *) NULL) {
	    fprintf(stderr, "%s: could not find a valid user name\n",progname);
	    exit(2);
    }
    memset(usernamebuf, 0, sizeof(usernamebuf));
    (void) strncpy(usernamebuf, username, NAMEDATALEN);
    username = usernamebuf;

    /*
     *  Determine the hostname of the database server.  Try to avoid using
     * "localhost" if at all possible.
     */
    if (!PQhost && !(PQhost = getenv("PGHOST")))
	    PQhost = "localhost";
    if (!strcmp(PQhost, "localhost")) {
	    if (gethostname(hostbuf, MAXHOSTNAMELEN) != -1)
		    PQhost = hostbuf;
    }


    /* find database */
    if (!(dbname = argv[optind]) &&
	!(dbname = getenv("DATABASE")) &&
	!(dbname = username)) {
	    fprintf(stderr, "%s: no database name specified\n",progname);
	    exit (2);
    }

    PQsetdb(dbname);

    /* make sure things are ok before giving users a warm welcome! */
    check_conn_and_db();

    if (g_outputSQL) {
	strcpy(g_comment_start,"-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");
    } else {
	strcpy(g_comment_start,"/* ");
	strcpy(g_comment_end,"*/ ");
	strcpy(g_opaque_type, "any");
    }

    g_last_builtin_oid = findLastBuiltinOid();
	

if (g_verbose) 
    fprintf(stderr, "%s last builtin oid is %d %s\n", 
	    g_comment_start, g_last_builtin_oid, g_comment_end);

    tblinfo = dumpSchema(g_fout, &numTables);
    
    if (!schemaOnly) {

if (g_verbose) {
    fprintf(stderr, "%s dumping out the contents of each table %s\n",
	    g_comment_start, g_comment_end );
    fprintf(stderr, "%s the output language is %s %s\n",
	    g_comment_start,
	    (g_outputSQL) ? "SQL" : "POSTQUEL",
	    g_comment_end);
}

      dumpClasses(tblinfo, numTables, g_fout); 
    }     

    fflush(g_fout);
    fclose(g_fout);
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
    char* res;
    PortalBuffer* pbuf;
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

    PQexec("begin");
    
   /* find all base types */
   /* we include even the built-in types 
      because those may be used as array elements by user-defined types */
   /* we filter out the built-in types when 
      we dump out the types */

/*
    sprintf(query, "SELECT oid, typowner,typname, typlen, typprtlen, typinput, typoutput, typreceive, typsend, typelem, typdelim, typdefault, typrelid,typbyval from pg_type");
*/
    sprintf(query, "retrieve (t.oid, t.typowner, t.typname, t.typlen, t.typprtlen, t.typinput, t.typoutput, t.typreceive, t.typsend, t.typelem, t.typdelim, t.typdefault, t.typrelid, t.typbyval) from t in pg_type");
    

    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);
    
    tinfo = (TypeInfo*)malloc(ntups * sizeof(TypeInfo));

    i_oid = PQfnumberGroup(pbuf,0,"oid");
    i_typowner = PQfnumberGroup(pbuf,0,"typowner");
    i_typname = PQfnumberGroup(pbuf,0,"typname");
    i_typlen = PQfnumberGroup(pbuf,0,"typlen");
    i_typprtlen = PQfnumberGroup(pbuf,0,"typprtlen");
    i_typinput = PQfnumberGroup(pbuf,0,"typinput");
    i_typoutput = PQfnumberGroup(pbuf,0,"typoutput");
    i_typreceive = PQfnumberGroup(pbuf,0,"typreceive");
    i_typsend = PQfnumberGroup(pbuf,0,"typsend");
    i_typelem = PQfnumberGroup(pbuf,0,"typelem");
    i_typdelim = PQfnumberGroup(pbuf,0,"typdelim");
    i_typdefault = PQfnumberGroup(pbuf,0,"typdefault");
    i_typrelid = PQfnumberGroup(pbuf,0,"typrelid");
    i_typbyval = PQfnumberGroup(pbuf,0,"typbyval");

    for (i=0;i<ntups;i++) {
	tinfo[i].oid = strdup(PQgetvalue(pbuf,i,i_oid));
	tinfo[i].typowner = strdup(PQgetvalue(pbuf,i,i_typowner));
	tinfo[i].typname = strdup(PQgetvalue(pbuf,i,i_typname));
	tinfo[i].typlen = strdup(PQgetvalue(pbuf,i,i_typlen));
	tinfo[i].typprtlen = strdup(PQgetvalue(pbuf,i,i_typprtlen));
	tinfo[i].typinput = strdup(PQgetvalue(pbuf,i,i_typinput));
	tinfo[i].typoutput = strdup(PQgetvalue(pbuf,i,i_typoutput));
	tinfo[i].typreceive = strdup(PQgetvalue(pbuf,i,i_typreceive));
	tinfo[i].typsend = strdup(PQgetvalue(pbuf,i,i_typsend));
	tinfo[i].typelem = strdup(PQgetvalue(pbuf,i,i_typelem));
	tinfo[i].typdelim = strdup(PQgetvalue(pbuf,i,i_typdelim));
	tinfo[i].typdefault = strdup(PQgetvalue(pbuf,i,i_typdefault));
	tinfo[i].typrelid = strdup(PQgetvalue(pbuf,i,i_typrelid));

	if (strcmp(PQgetvalue(pbuf,i,i_typbyval), "f") == 0)
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

    PQexec("end");
    PQclear(res+1);
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
    char *res;
    PortalBuffer *pbuf;
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
    PQexec("begin");
/*
    sprintf(query, "SELECT oid, oprname, oprkind, oprcode, oprleft, oprright, oprcom, oprnegate, oprrest, oprjoin, oprcanhash, oprlsortop, oprrsortop from pg_operator");
*/
    sprintf(query, "retrieve (o.oid, o.oprname, o.oprkind, o.oprcode, o.oprleft, o.oprright, o.oprcom, o.oprnegate, o.oprrest, o.oprjoin, o.oprcanhash, o.oprlsortop, o.oprrsortop) from o in pg_operator");


    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);
    *numOprs = ntups;

    oprinfo = (OprInfo*)malloc(ntups * sizeof(OprInfo));

    i_oid = PQfnumberGroup(pbuf,0,"oid");
    i_oprname = PQfnumberGroup(pbuf,0,"oprname");
    i_oprkind = PQfnumberGroup(pbuf,0,"oprkind");
    i_oprcode = PQfnumberGroup(pbuf,0,"oprcode");
    i_oprleft = PQfnumberGroup(pbuf,0,"oprleft");
    i_oprright = PQfnumberGroup(pbuf,0,"oprright");
    i_oprcom = PQfnumberGroup(pbuf,0,"oprcom");
    i_oprnegate = PQfnumberGroup(pbuf,0,"oprnegate");
    i_oprrest = PQfnumberGroup(pbuf,0,"oprrest");
    i_oprjoin = PQfnumberGroup(pbuf,0,"oprjoin");
    i_oprcanhash = PQfnumberGroup(pbuf,0,"oprcanhash");
    i_oprlsortop = PQfnumberGroup(pbuf,0,"oprlsortop");
    i_oprrsortop = PQfnumberGroup(pbuf,0,"oprrsortop");

    for (i=0;i<ntups;i++) {
	oprinfo[i].oid = strdup(PQgetvalue(pbuf,i,i_oid));
	oprinfo[i].oprname = strdup(PQgetvalue(pbuf,i,i_oprname));
	oprinfo[i].oprkind = strdup(PQgetvalue(pbuf,i,i_oprkind));
	oprinfo[i].oprcode = strdup(PQgetvalue(pbuf,i,i_oprcode));
	oprinfo[i].oprleft = strdup(PQgetvalue(pbuf,i,i_oprleft));
	oprinfo[i].oprright = strdup(PQgetvalue(pbuf,i,i_oprright));
	oprinfo[i].oprcom = strdup(PQgetvalue(pbuf,i,i_oprcom));
	oprinfo[i].oprnegate = strdup(PQgetvalue(pbuf,i,i_oprnegate));
	oprinfo[i].oprrest = strdup(PQgetvalue(pbuf,i,i_oprrest));
	oprinfo[i].oprjoin = strdup(PQgetvalue(pbuf,i,i_oprjoin));
	oprinfo[i].oprcanhash = strdup(PQgetvalue(pbuf,i,i_oprcanhash));
	oprinfo[i].oprlsortop = strdup(PQgetvalue(pbuf,i,i_oprlsortop));
	oprinfo[i].oprrsortop = strdup(PQgetvalue(pbuf,i,i_oprrsortop));
    }

    PQclear(res+1);
    PQexec("end");

    return oprinfo;
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
    char* res;
    PortalBuffer *pbuf;
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

    PQexec("begin");
/*
    sprintf(query, 
	    "SELECT oid, aggname, aggtransfn1, aggtransfn2, aggfinalfn, aggtranstype1, aggbasetype, aggtranstype2, agginitval1, agginitval2 from pg_aggregate;");
*/
    sprintf(query, 
	    "retrieve (a.oid, a.aggname, a.aggtransfn1, a.aggtransfn2, a.aggfinalfn, a.aggtranstype1, a.aggbasetype, a.aggtranstype2, a.agginitval1, a.agginitval2) from a in pg_aggregate");

    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);
    *numAggs = ntups;

    agginfo = (AggInfo*)malloc(ntups * sizeof(AggInfo));
    
    i_oid = PQfnumberGroup(pbuf,0,"oid");
    i_aggname = PQfnumberGroup(pbuf,0,"aggname");
    i_aggtransfn1 = PQfnumberGroup(pbuf,0,"aggtransfn1");
    i_aggtransfn2 = PQfnumberGroup(pbuf,0,"aggtransfn2");
    i_aggfinalfn = PQfnumberGroup(pbuf,0,"aggfinalfn");
    i_aggtranstype1 = PQfnumberGroup(pbuf,0,"aggtranstype1");
    i_aggbasetype = PQfnumberGroup(pbuf,0,"aggbasetype");
    i_aggtranstype2 = PQfnumberGroup(pbuf,0,"aggtranstype2");
    i_agginitval1 = PQfnumberGroup(pbuf,0,"agginitval1");
    i_agginitval2 = PQfnumberGroup(pbuf,0,"agginitval2");

    for (i=0;i<ntups;i++) {
	agginfo[i].oid = strdup(PQgetvalue(pbuf,i,i_oid));
	agginfo[i].aggname = strdup(PQgetvalue(pbuf,i,i_aggname));
	agginfo[i].aggtransfn1 = strdup(PQgetvalue(pbuf,i,i_aggtransfn1));
	agginfo[i].aggtransfn2 = strdup(PQgetvalue(pbuf,i,i_aggtransfn2));
	agginfo[i].aggfinalfn = strdup(PQgetvalue(pbuf,i,i_aggfinalfn));
	agginfo[i].aggtranstype1 = strdup(PQgetvalue(pbuf,i,i_aggtranstype1));
	agginfo[i].aggbasetype = strdup(PQgetvalue(pbuf,i,i_aggbasetype));
	agginfo[i].aggtranstype2 = strdup(PQgetvalue(pbuf,i,i_aggtranstype2));
	agginfo[i].agginitval1 = strdup(PQgetvalue(pbuf,i,i_agginitval1));
	agginfo[i].agginitval2 = strdup(PQgetvalue(pbuf,i,i_agginitval2));
    }

    PQclear(res+1);
    PQexec("end");

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
    char* res;
    PortalBuffer *pbuf;
    int ntups;
    int i, j;
    char query[MAXQUERYLEN];
    FuncInfo *finfo;
    char *proargtypes;

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

    PQexec("begin");

/*
    sprintf(query, 
	    "SELECT oid, proname, proowner, prolang, pronargs, prorettype, proretset, proargtypes, prosrc, probin from pg_proc where oid > '%d'::oid", 
	    g_last_builtin_oid);
*/
    sprintf(query, 
	    "retrieve (f.oid, f.proname, f.proowner, f.prolang, f.pronargs, f.prorettype, f.proretset, f.proargtypes, f.prosrc, f.probin) from f in pg_proc where f.oid > \"%d\"::oid", 
	    g_last_builtin_oid);

    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);
    
    *numFuncs = ntups;

    finfo = (FuncInfo*)malloc(ntups * sizeof(FuncInfo));

    i_oid = PQfnumberGroup(pbuf,0,"oid");
    i_proname = PQfnumberGroup(pbuf,0,"proname");
    i_proowner = PQfnumberGroup(pbuf,0,"proowner");
    i_prolang = PQfnumberGroup(pbuf,0,"prolang");
    i_pronargs = PQfnumberGroup(pbuf,0,"pronargs");
    i_proargtypes = PQfnumberGroup(pbuf,0,"proargtypes");
    i_prorettype = PQfnumberGroup(pbuf,0,"prorettype");
    i_proretset = PQfnumberGroup(pbuf,0,"proretset");
    i_prosrc = PQfnumberGroup(pbuf,0,"prosrc");
    i_probin = PQfnumberGroup(pbuf,0,"probin");
    
    for (i=0;i<ntups;i++) {
	finfo[i].oid = strdup(PQgetvalue(pbuf,i,i_oid));
	finfo[i].proname = strdup(PQgetvalue(pbuf,i,i_proname));
	finfo[i].proowner = strdup(PQgetvalue(pbuf,i,i_proowner));

	finfo[i].prosrc = checkForQuote(PQgetvalue(pbuf,i,i_prosrc));
	finfo[i].probin = strdup(PQgetvalue(pbuf,i,i_probin));

	finfo[i].prorettype = strdup(PQgetvalue(pbuf,i,i_prorettype));
	finfo[i].retset = (strcmp(PQgetvalue(pbuf,i,i_proretset),"t") == 0);
	finfo[i].nargs = atoi(PQgetvalue(pbuf,i,i_pronargs));
	finfo[i].lang = (atoi(PQgetvalue(pbuf,i,i_prolang)) == C_PROLANG_OID);

	parseArgTypes(finfo[i].argtypes, PQgetvalue(pbuf,i,i_proargtypes));

	finfo[i].dumped = 0;
    }

    PQclear(res+1);
    PQexec("end");

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
    char* res;
    PortalBuffer* pbuf;
    int ntups;
    int i, j;
    char query[MAXQUERYLEN];
    TableInfo *tblinfo;
    
    int i_oid;
    int i_relname;
    int i_relarch;

    /* find all the user-defined tables (no indices and no catalogs),
     ordering by oid is important so that we always process the parent
     tables before the child tables when traversing the tblinfo* */
    PQexec("begin");
/*
    sprintf(query, 
	    "SELECT oid, relname, relarch from pg_class where relkind = 'r' and relname !~ '^pg_' order by oid;");
*/
    sprintf(query, 
	    "retrieve (r.oid, r.relname, r.relarch) from r in pg_class where r.relkind = \"r\" and r.relname !~ \"^pg_\" and r.relname !~ \"^Xinv\" sort by oid");

    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);

    *numTables = ntups;

    tblinfo = (TableInfo*)malloc(ntups * sizeof(TableInfo));

    i_oid = PQfnumberGroup(pbuf,0,"oid");
    i_relname = PQfnumberGroup(pbuf,0,"relname");
    i_relarch = PQfnumberGroup(pbuf,0,"relarch");

    for (i=0;i<ntups;i++) {
	tblinfo[i].oid = strdup(PQgetvalue(pbuf,i,i_oid));
	tblinfo[i].relname = strdup(PQgetvalue(pbuf,i,i_relname));
	tblinfo[i].relarch = strdup(PQgetvalue(pbuf,i,i_relarch));
    }

    PQclear(res+1);
    PQexec("end");

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
    char* res;
    PortalBuffer* pbuf;
    int ntups;
    int i;
    char query[MAXQUERYLEN];
    InhInfo *inhinfo;
    
    int i_inhrel;
    int i_inhparent;

    /* find all the inheritance information */
    PQexec("begin");
/*
    sprintf(query,  "SELECT inhrel, inhparent from pg_inherits");
*/
    sprintf(query,  "retrieve (i.inhrel, i.inhparent) from i in pg_inherits");

    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);

    *numInherits = ntups;

    inhinfo = (InhInfo*)malloc(ntups * sizeof(InhInfo));

    i_inhrel = PQfnumberGroup(pbuf,0,"inhrel");
    i_inhparent = PQfnumberGroup(pbuf,0,"inhparent");

    for (i=0;i<ntups;i++) {
	inhinfo[i].inhrel = strdup(PQgetvalue(pbuf,i,i_inhrel));
	inhinfo[i].inhparent = strdup(PQgetvalue(pbuf,i,i_inhparent));
    }

    PQclear(res+1);
    PQexec("end");
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
    char *res;
    PortalBuffer *pbuf;
    int ntups;

    for (i=0;i<numTables;i++)  {

	/* find all the user attributes and their types*/
	/* we must read the attribute names in attribute number order! */
	/* because we will use the attnum to index into the attnames array 
	   later */
/*
	sprintf(q,"SELECT a.attnum, a.attname, t.typname from pg_attribute a, pg_type t where a.attrelid = '%s' and a.atttypid = t.oid and a.attnum > 0 order by attnum",tblinfo[i].oid);
*/
if (g_verbose) 
    fprintf(stderr,"%s finding the attrs and types for table: %s %s\n",
	    g_comment_start,
	    tblinfo[i].relname,
	    g_comment_end);


	sprintf(q,"retrieve (a.attnum, a.attname, t.typname) from a in pg_attribute, t in pg_type where a.attrelid = \"%s\" and a.atttypid = t.oid and a.attnum > 0 sort by attnum",tblinfo[i].oid);

	res = PQexec(q);
	pbuf = PQparray(res+1);
	ntups = PQntuplesGroup(pbuf,0);

	i_attname = PQfnumberGroup(pbuf,0,"attname");
	i_typname = PQfnumberGroup(pbuf,0,"typname");

	tblinfo[i].numatts = ntups;
	tblinfo[i].attnames = (char**) malloc( ntups * sizeof(char*));
	tblinfo[i].out_attnames = (char**) malloc( ntups * sizeof(char*));
	tblinfo[i].typnames = (char**) malloc( ntups * sizeof(char*));
	tblinfo[i].inhAttrs = (int*) malloc (ntups * sizeof(int));
	tblinfo[i].parentRels = NULL;
	tblinfo[i].numParents = 0;
	for (j=0;j<ntups;j++) {
	    tblinfo[i].attnames[j] = strdup(PQgetvalue(pbuf,j,i_attname));
	    tblinfo[i].typnames[j] = strdup(PQgetvalue(pbuf,j,i_typname));
	    tblinfo[i].inhAttrs[j] = 0; /* this flag is set in flagInhAttrs()*/
	}
	PQclear(res+1);
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
    char *res;
    PortalBuffer *pbuf;
    int ntups;
    IndInfo *indinfo;

    int i_indexrelname;
    int i_indrelname;
    int i_indamname;
    int i_indproc;
    int i_indkey;
    int i_indclassname;
    
    /* find all the user-define indices.
       We do not handle partial indices.
       We also assume that only single key indices 

       this is a 5-way join !!
    */
       
    PQexec("begin");
/*
    sprintf(query,
	    "SELECT t1.relname as indexrelname, t2.relname as indrelname, i.indproc, i.indkey[0], o.opcname as indclassname, a.amname as indamname from pg_index i, pg_class t1, pg_class t2, pg_opclass o, pg_am a where t1.oid = i.indexrelid and t2.oid = i.indrelid and o.oid = i.indclass[0] and t1.relam = a.oid and i.indexrelid > '%d'::oid and t2.relname !~ '^pg_';",
	    g_last_builtin_oid);
*/

    sprintf(query,
	    "retrieve (indexrelname = t1.relname, indrelname = t2.relname, i.indproc, i.indkey[0], indclassname = o.opcname, indamname = a.amname) from i in pg_index, t1 in pg_class, t2 in pg_class, o in pg_opclass, a in pg_am where t1.oid = i.indexrelid and t2.oid = i.indrelid and o.oid = i.indclass[0] and t1.relam = a.oid and i.indexrelid > \"%d\"::oid and t2.relname !~ \"^pg_\" and t1.relname !~ \"^Xinx\"",
	    g_last_builtin_oid);

    res = PQexec(query);
    pbuf = PQparray(res+1);
    ntups = PQntuplesGroup(pbuf,0);

    *numIndices = ntups;

    indinfo = (IndInfo*)malloc(ntups * sizeof (IndInfo));

    i_indexrelname = PQfnumberGroup(pbuf,0,"indexrelname");
    i_indrelname = PQfnumberGroup(pbuf,0,"indrelname");
    i_indamname = PQfnumberGroup(pbuf,0,"indamname");
    i_indproc = PQfnumberGroup(pbuf,0,"indproc");
    i_indkey = PQfnumberGroup(pbuf,0,"indkey");
    i_indclassname = PQfnumberGroup(pbuf,0,"indclassname");

    for (i=0;i<ntups;i++) {
	indinfo[i].indexrelname = strdup(PQgetvalue(pbuf,i,i_indexrelname));
	indinfo[i].indrelname = strdup(PQgetvalue(pbuf,i,i_indrelname));
	indinfo[i].indamname = strdup(PQgetvalue(pbuf,i,i_indamname));
	indinfo[i].indproc = strdup(PQgetvalue(pbuf,i,i_indproc));
	indinfo[i].indkey = strdup(PQgetvalue(pbuf,i,i_indkey));
	indinfo[i].indclassname = strdup(PQgetvalue(pbuf,i,i_indclassname));
    }
    PQclear(res+1);
    PQexec("end");

    return indinfo;
}

/*
 * dumpTypes
 *    writes out to fout queries to recreate all the user-defined types
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

	if (g_outputSQL) {
	    sprintf(q,
		    "CREATE TYPE %s ( internallength = %s, externallength = %s, input = %s, output = %s, send = %s, receive = %s, default = '%s'",
		    tinfo[i].typname,
		    tinfo[i].typlen,
		    tinfo[i].typprtlen,
		    tinfo[i].typinput,
		    tinfo[i].typoutput,
		    tinfo[i].typsend,
		    tinfo[i].typreceive,
		    tinfo[i].typdefault);
	} else {
	    sprintf(q,
		    "define type %s ( internallength = %s, externallength = %s, input = %s, output = %s, send = %s, receive = %s, default = \"%s\"",
		    tinfo[i].typname,
		    (strcmp(tinfo[i].typlen, "-1") == 0) ? "variable" : tinfo[i].typlen,
		    (strcmp(tinfo[i].typprtlen, "-1") == 0) ? "variable " :tinfo[i].typprtlen,
		    tinfo[i].typinput,
		    tinfo[i].typoutput,
		    tinfo[i].typsend,
		    tinfo[i].typreceive,
		    tinfo[i].typdefault);
	}

	if (tinfo[i].isArray) {
	    char* elemType;

	    elemType = findTypeByOid(tinfo, numTypes, tinfo[i].typelem);
	    
	    if (g_outputSQL)
		sprintf(q,"%s, element = %s, delimiter = '%s'",
			q, elemType,tinfo[i].typdelim);
	    else
		sprintf(q,"%s, element = %s, delimiter = \"%s\"",
			q, elemType,tinfo[i].typdelim);
	}
	if (tinfo[i].passedbyvalue)
	    strcat(q,",passedbyvalue");
	else
	    strcat(q,")");

	if (g_outputSQL) 
	    strcat(q,";\n");
	else
	    strcat(q,"\\g\n");
	
	fputs(q,fout);
    }
    fflush(fout);
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
    char q[MAXQUERYLEN];
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

    if (g_outputSQL) {
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
if (finfo[i].lang != 1) {
    fprintf(stderr, 
	    "%s WARNING: text of function named %s is in POSTQUEL %s\n",
	    g_comment_start,
	    finfo[i].proname,
	    g_comment_end);
}

    } else {
	sprintf(q,"define function %s ( language = \"%s\", returntype = %s%s) arg is (",
		finfo[i].proname,
		(finfo[i].lang) ? "c" : "postquel",
		finfo[i].retset ? " setof " : "",
		findTypeByOid(tinfo, numTypes, finfo[i].prorettype)
		);

	for (j=0;j<finfo[i].nargs;j++) {
	    char* typname;
	    typname = findTypeByOid(tinfo, numTypes, finfo[i].argtypes[j]);
	    sprintf(q, "%s%s%s",
		    q,  
		(j > 0) ? "," : "",
		    typname);
	}
	sprintf(q,"%s ) as \"%s\"\\g\n",
		q, 
		(finfo[i].lang) ? finfo[i].probin : finfo[i].prosrc);
    }
    
    fputs(q,fout);
    fflush(fout);

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
    char comma[2];

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
	    sprintf(leftarg, ", %s = %s ",
		    (g_outputSQL) ? "LEFTARG" : "arg1",
		    findTypeByOid(tinfo, numTypes, oprinfo[i].oprleft));
	} 
	if (strcmp(oprinfo[i].oprkind, "l") == 0 || 
	    strcmp(oprinfo[i].oprkind, "b") == 0 ) {
	    sprintf(rightarg, ", %s = %s ",
		    (g_outputSQL) ? "RIGHTARG" : "arg2",
		    findTypeByOid(tinfo, numTypes, oprinfo[i].oprright));
	}
	if (strcmp(oprinfo[i].oprcom, "0") == 0) 
	    commutator[0] = '\0';
	else
	    sprintf(commutator,", commutator = %s ",
		    findOprByOid(oprinfo, numOperators, oprinfo[i].oprcom));

	if (strcmp(oprinfo[i].oprnegate, "0") == 0) 
	    negator[0] = '\0';
	else
	    sprintf(negator,", negator = %s ",
		    findOprByOid(oprinfo, numOperators, oprinfo[i].oprnegate));

	if (strcmp(oprinfo[i].oprrest, "-") == 0)
	    restrict[0] = '\0';
	else
	    sprintf(restrict,", restrict = %s ", oprinfo[i].oprrest);
		    
	if (strcmp(oprinfo[i].oprjoin,"-") == 0)
	    join[0] = '\0';
	else
	    sprintf(join,", join = %s ", oprinfo[i].oprjoin);
		    
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

	if (g_outputSQL)  {
	    sprintf(q,
		    "CREATE OPERATOR %s (PROCEDURE = %s %s %s %s %s %s %s %s %s);\n ",
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
	} else
	    sprintf(q,
		    "define operator %s (procedure = %s %s %s %s %s %s %s %s %s)\\g\n ",
		    oprinfo[i].oprname,
		    oprinfo[i].oprcode,
		    leftarg,
		    rightarg,
		    commutator,
		    negator,
		    restrict,
		    (strcmp(oprinfo[i].oprcanhash, "t")) ? ", hashes" : "",
		    join,
		    sortop);

	fputs(q,fout);
    }
    fflush(fout);

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
    char *basetype;
    char *stype1;
    char *stype2;
    char comma1[2], comma2[2];

    for (i=0;i<numAggs;i++) {
	/* skip all the builtin oids */
	if (atoi(agginfo[i].oid) < g_last_builtin_oid)
	    continue;

	if ( strcmp(agginfo[i].aggtransfn1, "-") == 0) 
	    sfunc1[0] = '\0';
	else {
	    sprintf(sfunc1, 
		    "sfunc1 = %s, basetype = %s, stype1 = %s",
		    agginfo[i].aggtransfn1,
		    findTypeByOid(tinfo,numTypes,agginfo[i].aggbasetype),
		    findTypeByOid(tinfo,numTypes,agginfo[i].aggtranstype1));
	    if (agginfo[i].agginitval1) {
		if (g_outputSQL)
		    sprintf(sfunc1, "%s ,INITCOND1 = '%s'",
			    sfunc1, agginfo[i].agginitval1);
		else
		    sprintf(sfunc1, "%s ,initcond1 = \"%s\"",
			    sfunc1, agginfo[i].agginitval1);

	    }
	    
	}

	if ( strcmp(agginfo[i].aggtransfn2, "-") == 0) 
	    sfunc2[0] = '\0';
	else {
	    sprintf(sfunc2, 
		    "sfunc2 = %s, stype2 = %s",
		    agginfo[i].aggtransfn2,
		    findTypeByOid(tinfo,numTypes,agginfo[i].aggtranstype2));
	    if (agginfo[i].agginitval2) {
		if (g_outputSQL)
		    sprintf(sfunc2,"%s ,initcond2 = '%s'",
			    sfunc2, agginfo[i].agginitval2);
		else
		    sprintf(sfunc2,"%s ,initcond2 = \"%s\"",
			    sfunc2, agginfo[i].agginitval2);

	    }
	}
	
	if ( strcmp(agginfo[i].aggfinalfn, "-") == 0)
	    finalfunc[0] = '\0';
	else {
	    sprintf(finalfunc, "finalfunc = %s", agginfo[i].aggfinalfn);
	}
	if (sfunc1[0] != '\0' && sfunc2[0] != '\0') {
	    comma1[0] = ','; comma1[1] = '\0';
	} else
	    comma1[0] = '\0';

	if (finalfunc[0] != '\0' && (sfunc1[0] != '\0' || sfunc2[0] != '\0')) {
	    comma2[0] = ',';comma2[1] = '\0';
	} else
	    comma2[0] = '\0';

	if (g_outputSQL) {
	    sprintf(q,"CREATE AGGREGATE %s ( %s %s %s %s %s );\n",
		    agginfo[i].aggname,
		    sfunc1,
		    comma1,
		    sfunc2,
		    comma2,
		    finalfunc);
	} else {
	    sprintf(q,"define aggregate %s ( %s %s %s %s %s )\\g\n",
		    agginfo[i].aggname,
		    sfunc1,
		    comma1,
		    sfunc2,
		    comma2,
		    finalfunc);
	}
	
	fputs(q,fout);
    }
    fflush(fout);
}

/*
 * dumpTables:
 *    write out to fout all the user-define tables
 */

void
dumpTables(FILE* fout, TableInfo *tblinfo, int numTables,
	   InhInfo *inhinfo, int numInherits,
	   TypeInfo *tinfo, int numTypes)
{
    int i,j,k;
    char q[MAXQUERYLEN];
    char **parentRels;  /* list of names of parent relations */
    int numParents;
    char *res;
    PortalBuffer *pbuf;
    int ntups;
    int actual_atts; /* number of attrs in this CREATE statment */
    char *archiveMode;

    for (i=0;i<numTables;i++) {
	parentRels = tblinfo[i].parentRels;
	numParents = tblinfo[i].numParents;

	if (g_outputSQL) {
	    sprintf(q, "CREATE TABLE %s (", tblinfo[i].relname);
	} else {
	    sprintf(q, "create %s (", tblinfo[i].relname);
	}
	
	actual_atts = 0;
	for (j=0;j<tblinfo[i].numatts;j++) {
	    if (tblinfo[i].inhAttrs[j] == 0) {
		if (g_outputSQL) {
		    sprintf(q, "%s%s%s %s",
			    q,
			    (actual_atts > 0) ? ", " : "",
			    tblinfo[i].attnames[j],
			    tblinfo[i].typnames[j]);
		}
		else { 
		    sprintf(q, "%s%s %s = %s",
			    q,
			    (actual_atts > 0) ? ", " : "",
			    tblinfo[i].attnames[j],
			    tblinfo[i].typnames[j]);

		}
		actual_atts++;
	    }
	}

	strcat(q,")");

	if (numParents > 0) {
	    int oa = 0; /* index for the out_attnames array */
	    int l;
	    int parentInd;

	    sprintf(q, "%s inherits ( ",q);
	    for (k=0;k<numParents;k++){
		sprintf(q, "%s%s%s",
			q,
			(k>0) ? ", " : "",
			parentRels[k]);
		parentInd = findTableByName(tblinfo,numTables,parentRels[k]);

		/* the out_attnames are in order of the out_attnames
		   of the parent tables */
		for (l=0; l<tblinfo[parentInd].numatts;l++)
		    tblinfo[i].out_attnames[oa++] =
			tblinfo[parentInd].out_attnames[l];
	    }

	    /* include non-inherited attrs in out_attnames also,
	       oa should never exceed numatts */
	    for (l=0; l < tblinfo[i].numatts && oa < tblinfo[i].numatts ; l++)
		if (tblinfo[i].inhAttrs[l] == 0) {
		    tblinfo[i].out_attnames[oa++] = 
			tblinfo[i].attnames[l];
		}

	    strcat(q,")");
	}  else { /* for non-inherited tables, out_attnames 
		     and attnames are the same  */
	    tblinfo[i].out_attnames = tblinfo[i].attnames;
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
	    
	if (g_outputSQL) {
	    sprintf(q, "%s archive = %s;\n",
		    q,
		    archiveMode);
	} else {
	    sprintf(q, "%s archive = %s\\g\n",
		    q,
		    archiveMode);
	}
	    
	fputs(q,fout);
    }
    fflush(fout);
}

/*
 * dumpIndices:
 *    write out to fout all the user-define indices
 */
void 
dumpIndices(FILE* fout, IndInfo* indinfo, int numIndices,
	    TableInfo* tblinfo, int numTables)
{
    int i,j;
    int tableInd;
    char *attname;  /* the name of the indexed attribute  */
    char *funcname; /* the name of the function to compute the index key from*/
    int indkey;

    char q[MAXQUERYLEN];
    char *res;
    PortalBuffer *pbuf;

    for (i=0;i<numIndices;i++) {
	tableInd = findTableByName(tblinfo, numTables,
				   indinfo[i].indrelname);
	indkey = atoi(indinfo[i].indkey) - 1; 
	attname = tblinfo[tableInd].attnames[indkey];
	if (strcmp(indinfo[i].indproc,"0") == 0) {
	    funcname = NULL;
	} else {
	    /* the funcname is an oid which we use to 
	       find the name of the pg_proc.  We need to do this
	       because getFuncs() only reads in the user-defined funcs
	       not all the funcs.  We might not find what we want
	       by looking in FuncInfo**/
	    sprintf(q,
		    "retrieve(p.proname) from p in pg_proc where p.oid = \"%s\"::oid",
		    indinfo[i].indproc);
	    res = PQexec(q);
	    pbuf = PQparray(res+1);
	    funcname = strdup(PQgetvalue(pbuf,0,
					 PQfnumberGroup(pbuf,0,"proname")));
	    PQclear(res+1);
	}
	if (g_outputSQL) {
	    sprintf(q,"CREATE INDEX %s on %s using %s (",
		    indinfo[i].indexrelname,
		    indinfo[i].indrelname,
		    indinfo[i].indamname);
	} else {
	    sprintf(q,"define index %s on %s using %s (",
		    indinfo[i].indexrelname,
		    indinfo[i].indrelname,
		    indinfo[i].indamname);

	}
	if (funcname) {
	    sprintf(q, "%s %s(%s) %s",
		    q,funcname, attname, indinfo[i].indclassname);
	    free(funcname); 
	} else
	    sprintf(q, "%s %s %s",
		    q,attname,indinfo[i].indclassname);

	if (g_outputSQL) {
	    strcat(q,");\n");
	} else
	    strcat(q,")\\g\n");

	fputs(q,fout);
    }
    fflush(fout);
}


/*
 * dumpClasses -
 *    dump the contents of all the classes.
 */
void
dumpClasses(TableInfo *tblinfo, int numTables, FILE *fout)
{
    char query[255];
    char *res;
    int i,j;

    int *attrmap; /* this is an vector map of how the actual attributes
		     map to the corresponding output attributes.
		     This is necessary because of a difference between
		     SQL and POSTQUEL in the order of inherited attributes */

    for(i = 0; i < numTables; i++) {
	char *classname = tblinfo[i].relname;

	if (g_outputSQL) 
	    fprintf(fout, "copy %s from stdin;\n", classname);
	else
	    fprintf(fout, "copy %s from stdin\\g\n", classname);

	sprintf(query, "retrieve (p.all) from p in %s", classname);
	res = PQexec(query);

	attrmap = (int*)malloc(tblinfo[i].numatts * sizeof(int));
	if (tblinfo[i].numParents == 0) {
	    /* table with no inheritance use an identity mapping */
	    for (j=0;j<tblinfo[i].numatts;j++)
		attrmap[j] = j;
	} else {
	    int n = tblinfo[i].numatts;
	    for (j=0;j < n;j++) {
		attrmap[j] = strInArray(tblinfo[i].attnames[j],
				       tblinfo[i].out_attnames,
				       n);
	    }
	}

/*
	{
	    int j;
	    for (j=0;j<tblinfo[i].numatts;j++) {
		fprintf(stderr,":%s\t",tblinfo[i].out_attnames[j]);
	    }
	    fprintf(stderr,"\n");
	}
*/

	fflush(stdout);
	fflush(stderr);
	switch (*res) {
	case 'P':
	    dumpTuples(&(res[1]), fout, attrmap);
	    PQclear(&(res[1]));
	    break;
	case 'E':
	case 'R':
	    fprintf(stderr, "Error while dumping %s\n", classname);
	    exit(1);
	    break;
	}

	fprintf(fout, ".\n");
	free(attrmap);
    }
}

/*
 * dumpTuples --
 *    prints out the tuples in ASCII representaiton. The output is a valid
 *    input to COPY FROM stdin.
 *
 *    We only need to do this for POSTGRES 4.2 databases since the
 *    COPY TO statement doesn't escape newlines properly. It's been fixed
 *    in Postgres95.
 * 
 * the attrmap passed in tells how to map the attributes copied in to the
 * attributes copied out
 */
void
dumpTuples(char *portalname, FILE *fout, int* attrmap)
{
    PortalBuffer *pbuf;
    int i, j, k;
    int m, n, t;
    char **outVals = NULL; /* values to copy out */

    /* Now to examine all tuples fetched. */
    pbuf = PQparray(portalname);

    n = PQntuplesGroup(pbuf,0); /* always assume only one group */
    m = PQnfieldsGroup(pbuf,0);

    if ( m > 0 ) {
	/*
	 * Print out the tuples but only print tuples with at least
	 * 1 field.
	 */
	outVals = (char**)malloc(m * sizeof(char*));
    
	for (j = 0; j < n; j++) {
	    for (k = 0; k < m; k++) {
		outVals[attrmap[k]] = PQgetvalue(pbuf, j, k);
	    }
	    for (k = 0; k < m; k++) {
		char *pval = outVals[k];

		if (k!=0)
		    fputc('\t', fout);	/* delimiter for attribute */

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
	    fputc('\n', fout);	/* delimiter for a tuple */
	}
	free (outVals);
    }
    
}



/*
 * findLastBuiltInOid -
 * find the last built in oid 
 * we do this by looking up the oid of 'template1' in pg_database,
 * this is probably not foolproof but comes close 
*/

int
findLastBuiltinOid()
{
	char *res;
	PortalBuffer* pbuf;
	int ntups;
	int last_oid;

	res = PQexec("retrieve (d.oid) from d in pg_database where d.datname = \"template1\"");
	pbuf = PQparray(res+1);
	ntups = PQntuplesGroup(pbuf,0);
	if (ntups != 1) {
	    fprintf(stderr,"pg_dump: couldn't find the template1 database.  You are really hosed\nGiving up\n");
	    exit(2);
	}
	return (atoi(PQgetvalue(pbuf,0, PQfnumberGroup(pbuf,0,"oid"))));

}


/*
 * checkForQuote:
 *    checks a string for quote characters and backslashes them
 */
char*
checkForQuote(char* s)
{
    char *r;
    char c;
    char *result;

    int j = 0;

    r = malloc(strlen(s)*3 + 1);  /* definitely long enough */

    while ( (c = *s) != '\0') {

	if (c == '\"') {
	    /* backslash the double quotes */
	    if (g_outputSQL) {
		r[j++] = '\\'; 
		c = '\'';
	    } else {
		r[j++] = '\\';
		r[j++] = '\\';
	    }
	}
	r[j++] = c;
	s++;
    }
    r[j] = '\0';

    result = strdup(r);
    free(r);

    return result;
    
}
