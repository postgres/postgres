/*-------------------------------------------------------------------------
 *
 * foreign.h
 *	  support for foreign-data wrappers, servers and user mappings.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/foreign/foreign.h,v 1.2 2009/01/01 17:23:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FOREIGN_H
#define FOREIGN_H

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"


/* Helper for obtaining username for user mapping */
#define MappingUserName(userid) \
	(OidIsValid(userid) ? GetUserNameFromId(userid) : "public")


/*
 * Generic option types for validation.
 * NB! Thes are treated as flags, so use only powers of two here.
 */
typedef enum {
	InvalidOpt = 0,
	ServerOpt = 1,				/* options applicable to SERVER */
	UserMappingOpt = 2,			/* options for USER MAPPING */
	FdwOpt = 4,					/* options for FOREIGN DATA WRAPPER */
} GenericOptionFlags;

typedef struct ForeignDataWrapperLibrary ForeignDataWrapperLibrary;

typedef struct ForeignDataWrapper
{
	Oid		fdwid;				/* FDW Oid */
	Oid		owner;				/* FDW owner user Oid */
	char   *fdwname;			/* Name of the FDW */
	char   *fdwlibrary;			/* Library name */
	List   *options;			/* fdwoptions as DefElem list */

	ForeignDataWrapperLibrary *lib;	/* interface to the FDW functions */
} ForeignDataWrapper;

typedef struct ForeignServer
{
	Oid		serverid;			/* server Oid */
	Oid		fdwid;				/* foreign-data wrapper */
	Oid		owner;				/* server owner user Oid */
	char	*servername;		/* name of the server */
	char	*servertype;		/* server type, optional */
	char	*serverversion;		/* server version, optional */
	List	*options;			/* srvoptions as DefElem list */
} ForeignServer;

typedef struct UserMapping
{
	Oid		userid;				/* local user Oid */
	Oid		serverid;			/* server Oid */
	List	*options;			/* useoptions as DefElem list */
} UserMapping;


/*
 * Foreign-data wrapper library function types.
 */
typedef void (*OptionListValidatorFunc)(ForeignDataWrapper *,
										GenericOptionFlags,
										List *);

/*
 * Interface functions to the foreign-data wrapper. This is decoupled
 * from the FDW as there maybe several FDW-s accessing the same library.
 */
struct ForeignDataWrapperLibrary
{
	char 	   *libname;		/* name of the library file */

	OptionListValidatorFunc	validateOptionList;
};


extern ForeignServer *GetForeignServer(Oid serverid);
extern ForeignServer *GetForeignServerByName(const char *name, bool missing_ok);
extern Oid GetForeignServerOidByName(const char *name, bool missing_ok);
extern UserMapping *GetUserMapping(Oid userid, Oid serverid);
extern ForeignDataWrapper *GetForeignDataWrapper(Oid fdwid);
extern ForeignDataWrapper *GetForeignDataWrapperByName(const char *name,
													   bool missing_ok);
extern Oid GetForeignDataWrapperOidByName(const char *name, bool missing_ok);
extern ForeignDataWrapperLibrary *GetForeignDataWrapperLibrary(const char *libname);

/* Foreign data wrapper interface functions */
extern void _pg_validateOptionList(ForeignDataWrapper *fdw,
								   GenericOptionFlags flags, List *options);

#endif /* FOREIGN_H */
