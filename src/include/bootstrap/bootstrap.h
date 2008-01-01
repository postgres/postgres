/*-------------------------------------------------------------------------
 *
 * bootstrap.h
 *	  include file for the bootstrapping code
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/bootstrap/bootstrap.h,v 1.50 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include "nodes/execnodes.h"

/*
 * MAXATTR is the maximum number of attributes in a relation supported
 * at bootstrap time (i.e., the max possible in a system table).
 */
#define MAXATTR 40

typedef struct hashnode
{
	int			strnum;			/* Index into string table */
	struct hashnode *next;
} hashnode;


extern Relation boot_reldesc;
extern Form_pg_attribute attrtypes[MAXATTR];
extern int	numattr;
extern void AuxiliaryProcessMain(int argc, char *argv[]);

extern void index_register(Oid heap, Oid ind, IndexInfo *indexInfo);

extern void err_out(void);
extern void InsertOneTuple(Oid objectid);
extern void closerel(char *name);
extern void boot_openrel(char *name);
extern char *LexIDStr(int ident_num);

extern void DefineAttr(char *name, char *type, int attnum);
extern void InsertOneValue(char *value, int i);
extern void InsertOneNull(int i);
extern char *MapArrayTypeName(char *s);
extern char *CleanUpStr(char *s);
extern int	EnterString(char *str);
extern void build_indices(void);

extern void boot_get_type_io_data(Oid typid,
					  int16 *typlen,
					  bool *typbyval,
					  char *typalign,
					  char *typdelim,
					  Oid *typioparam,
					  Oid *typinput,
					  Oid *typoutput);

extern int	boot_yyparse(void);

extern int	boot_yylex(void);
extern void boot_yyerror(const char *str);

typedef enum
{
	CheckerProcess,
	BootstrapProcess,
	StartupProcess,
	BgWriterProcess,
	WalWriterProcess
} AuxProcType;

#endif   /* BOOTSTRAP_H */
