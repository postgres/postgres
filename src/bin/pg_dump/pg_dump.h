/*-------------------------------------------------------------------------
 *
 * pg_dump.h
 *    header file for the pg_dump utility
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_dump.h,v 1.8 1996/11/26 03:19:49 bryanh Exp $
 *
 * Modifications - 6/12/96 - dave@bensoft.com - version 1.13.dhb.2
 *
 *   - Fixed dumpTable output to output lengths for char and varchar types!
 *   - Added single. quote to twin single quote expansion for 'insert' string
 *     mode.
 *-------------------------------------------------------------------------
 */


/* The *Info data structures run-time C structures used to store
   system catalog information */
   
typedef struct _typeInfo {
    char* oid;
    char* typowner;
    char* typname;
    char* typlen;
    char* typprtlen;
    char* typinput;
    char* typoutput;
    char* typreceive;
    char* typsend;
    char* typelem;
    char* typdelim;
    char* typdefault;
    char* typrelid;
    int passedbyvalue;
    int isArray;
} TypeInfo;

typedef struct _funcInfo {
    char* oid;
    char* proname;
    char* proowner;
    int lang;  /* 1 if C, else SQL */
    int nargs;
    char* argtypes[8];  /* should be derived from obj/fmgr.h instead of hardwired*/
    char* prorettype;
    int retset; /* 1 if the function returns a set, 0 otherwise */
    char* prosrc;
    char* probin;
    int dumped; /* 1 if already dumped */
} FuncInfo;

typedef struct _tableInfo {
    char *oid;
    char *relname;
    char *relarch;
    int numatts;            /* number of attributes */
    int *inhAttrs;          /* an array of flags, one for each attribute
		              if the value is 1, then this attribute is
			      an inherited attribute */
    char **attnames;        /* the attribute names */
    char **typnames;        /* fill out attributes */
    int numParents;         /* number of (immediate) parent supertables */
    char **parentRels;      /* names of parent relations, NULL
			       if numParents == 0 */
    char **out_attnames;    /* the attribute names, in the order they would
			       be in, when the table is created in the
			       target query language.
			       this is needed because the SQL tables will
			       not have the same order of attributes as
			       the POSTQUEL tables */
    int *attlen;	    /* attribute lengths */
           
} TableInfo;

typedef struct _inhInfo {
    char *oid;
    char *inhrel;
    char *inhparent;
} InhInfo;

typedef struct _indInfo {
    char *indexrelname;  /* name of the secondary index class */
    char *indrelname;    /* name of the indexed heap class */
    char *indamname;     /* name of the access method (e.g. btree, rtree, etc.) */
    char *indproc;       /* oid of the function to compute the index, 0 if none*/
    char *indkey;        /* attribute number of the key attribute */
    char *indclassname;  /* name of the opclass of the key */
} IndInfo;

typedef struct _aggInfo {
    char *oid;
    char *aggname;
    char *aggtransfn1;
    char *aggtransfn2;
    char *aggfinalfn;
    char *aggtranstype1;
    char *aggbasetype;
    char *aggtranstype2;
    char *agginitval1;
    char *agginitval2;
} AggInfo;

typedef struct _oprInfo {
    char *oid;
    char *oprname;
    char *oprkind;   /* "b" = binary, "l" = left unary, "r" = right unary */
    char *oprcode;   /* operator function name */
    char *oprleft;   /* left operand type */
    char *oprright;  /* right operand type */
    char *oprcom;    /* oid of the commutator operator */
    char *oprnegate; /* oid of the negator operator */
    char *oprrest;   /* name of the function to calculate operator restriction
			selectivity */
    char *oprjoin;    /* name of the function to calculate operator join
			 selectivity */
    char *oprcanhash; /* can we use hash join strategy ? */
    char *oprlsortop; /* oid's of the left and right sort operators */
    char *oprrsortop;
} OprInfo;


/* global decls */
extern int g_verbose;  /* verbose flag */
extern int g_last_builtin_oid; /* value of the last builtin oid */
extern FILE *g_fout;     /* the script file */

/* placeholders for comment starting and ending delimiters */
extern char g_comment_start[10]; 
extern char g_comment_end[10]; 

extern char g_opaque_type[10]; /* name for the opaque type */

/* pg_dump is really two programs in one
    one version works with postgres v4r2
    and the other works with postgres95
    the common routines are declared here
*/
/*
 *  common utility functions 
*/

extern TableInfo* dumpSchema(FILE* fout, int *numTablesPtr, const char *tablename);
extern void dumpSchemaIdx(FILE* fout, int *numTablesPtr, const char *tablename,
				TableInfo* tblinfo, int numTables);

extern char* findTypeByOid(TypeInfo* tinfo, int numTypes, const char* oid);
extern char* findOprByOid(OprInfo *oprinfo, int numOprs, const char *oid);
extern int findFuncByName(FuncInfo* finfo, int numFuncs, const char* name);
extern char** findParentsByOid(TableInfo* tbinfo, int numTables,
			      InhInfo* inhinfo, int numInherits,
			      const char *oid, 
			      int *numParents);
extern int findTableByName(TableInfo *tbinfo, int numTables, const char *relname);
extern int findTableByOid(TableInfo *tbinfo, int numTables, const char *oid);
extern void flagInhAttrs(TableInfo* tbinfo, int numTables,
			   InhInfo* inhinfo, int numInherits);

extern void check_conn_and_db(void);
extern int strInArray(const char* pattern, char** arr, int arr_size);
extern void parseArgTypes(char **argtypes, const char* str);
extern int isArchiveName(const char*);

/*
 * version specific routines 
 */
extern TypeInfo* getTypes(int *numTypes);
extern FuncInfo* getFuncs(int *numFuncs);
extern AggInfo* getAggregates(int *numAggregates);
extern OprInfo* getOperators(int *numOperators);
extern TableInfo* getTables(int *numTables);
extern InhInfo* getInherits(int *numInherits);
extern void getTableAttrs(TableInfo* tbinfo, int numTables);
extern IndInfo* getIndices(int *numIndices);
extern void dumpTypes(FILE* fout, FuncInfo* finfo, int numFuncs,
		      TypeInfo* tinfo, int numTypes);
extern void dumpFuncs(FILE* fout, FuncInfo* finfo, int numFuncs,
		      TypeInfo *tinfo, int numTypes);
extern void dumpAggs(FILE* fout, AggInfo* agginfo, int numAggregates,
		     TypeInfo *tinfo, int numTypes);
extern void dumpOprs(FILE* fout, OprInfo* agginfo, int numOperators,
		     TypeInfo *tinfo, int numTypes);
extern void dumpOneFunc(FILE* fout, FuncInfo* finfo, int i,
			TypeInfo *tinfo, int numTypes);
extern void dumpTables(FILE* fout, TableInfo* tbinfo, int numTables,
		       InhInfo *inhinfo, int numInherits,
		       TypeInfo *tinfo, int numTypes, const char *tablename);
extern void dumpIndices(FILE* fout, IndInfo* indinfo, int numIndices,
			TableInfo* tbinfo, int numTables, const char *tablename);

extern void dumpClasses(TableInfo *tbinfo, int numTables, FILE *fout, 
                        const char *tablename, int oids);
extern void dumpTuples(PGresult *res, FILE *fout, int *attrmap);
extern void setMaxOid(FILE *fout);
extern char* checkForQuote(const char* s);
extern int findLastBuiltinOid(void);


/* largest query string size */
#define MAXQUERYLEN  5000

/* these voodoo constants are from the backend */
#define C_PROLANG_OID       13
