
/* File:            dlg_specific.h
 *
 * Description:     See "dlg_specific.c"
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __DLG_SPECIFIC_H__
#define __DLG_SPECIFIC_H__

#include "psqlodbc.h"
#include "connection.h"
#include  <windows.h>
#include  <windowsx.h>
#include  <odbcinst.h>
#include "resource.h"

/*	Unknown data type sizes */
#define UNKNOWNS_AS_MAX				0
#define UNKNOWNS_AS_DONTKNOW		1
#define UNKNOWNS_AS_LONGEST			2

/* INI File Stuff */
#define ODBC_INI     "ODBC.INI"         /* ODBC initialization file */
#define ODBCINST_INI "ODBCINST.INI"		/* ODBC Installation file */

#define INI_DSN           DBMS_NAME         /* Name of default Datasource in ini file (not used?) */
#define INI_KDESC         "Description"     /* Data source description */
#define INI_SERVER        "Servername"      /* Name of Server running the Postgres service */
#define INI_PORT          "Port"            /* Port on which the Postmaster is listening */ 
#define INI_DATABASE      "Database"        /* Database Name */
#define INI_USER          "Username"        /* Default User Name */
#define INI_PASSWORD      "Password"		/* Default Password */
#define INI_DEBUG         "Debug"			/* Debug flag */
#define INI_FETCH         "Fetch"			/* Fetch Max Count */
#define INI_SOCKET        "Socket"			/* Socket buffer size */
#define INI_READONLY      "ReadOnly"		/* Database is read only */
#define INI_COMMLOG       "CommLog"			/* Communication to backend logging */
#define INI_PROTOCOL      "Protocol"		/* What protocol (6.2) */
#define INI_OPTIMIZER     "Optimizer"		/* Use backend genetic optimizer */
#define INI_CONNSETTINGS  "ConnSettings"	/* Anything to send to backend on successful connection */
#define INI_UNIQUEINDEX   "UniqueIndex"		/* Recognize unique indexes */
#define INI_UNKNOWNSIZES  "UnknownSizes"	/* How to handle unknown result set sizes */

#define INI_USEDECLAREFETCH "UseDeclareFetch"		/* Use Declare/Fetch cursors */

/*	More ini stuff */
#define INI_TEXTASLONGVARCHAR		"TextAsLongVarchar"
#define INI_UNKNOWNSASLONGVARCHAR	"UnknownsAsLongVarchar"
#define INI_BOOLSASCHAR				"BoolsAsChar"
#define INI_MAXVARCHARSIZE			"MaxVarcharSize"
#define INI_MAXLONGVARCHARSIZE		"MaxLongVarcharSize"

#define INI_FAKEOIDINDEX			"FakeOidIndex"
#define INI_SHOWOIDCOLUMN			"ShowOidColumn"
#define INI_SHOWSYSTEMTABLES		"ShowSystemTables"
#define INI_EXTRASYSTABLEPREFIXES	"ExtraSysTablePrefixes"

/*	Connection Defaults */
#define DEFAULT_PORT					"5432"
#define DEFAULT_READONLY				1
#define DEFAULT_USEDECLAREFETCH			1
#define DEFAULT_TEXTASLONGVARCHAR		1
#define DEFAULT_UNKNOWNSASLONGVARCHAR	0
#define DEFAULT_BOOLSASCHAR				1
#define DEFAULT_OPTIMIZER				1		// disable
#define DEFAULT_UNIQUEINDEX				0		// dont recognize
#define DEFAULT_COMMLOG					0		// dont log
#define DEFAULT_UNKNOWNSIZES			UNKNOWNS_AS_MAX


#define DEFAULT_FAKEOIDINDEX			0
#define DEFAULT_SHOWOIDCOLUMN			0
#define DEFAULT_SHOWSYSTEMTABLES		0		// dont show system tables

#define DEFAULT_EXTRASYSTABLEPREFIXES	"dd_;"

/*  prototypes */
void updateGlobals(void);
void getGlobalDefaults(void);

void SetDlgStuff(HWND hdlg, ConnInfo *ci);
void GetDlgStuff(HWND hdlg, ConnInfo *ci);

int CALLBACK driver_optionsProc(HWND   hdlg,
                           WORD   wMsg,
                           WPARAM wParam,
                           LPARAM lParam);
int CALLBACK ds_optionsProc(HWND   hdlg,
                           WORD   wMsg,
                           WPARAM wParam,
                           LPARAM lParam);

void makeConnectString(char *connect_string, ConnInfo *ci);
void copyAttributes(ConnInfo *ci, char *attribute, char *value);
void getDSNdefaults(ConnInfo *ci);

void getDSNinfo(ConnInfo *ci, char overwrite);
void writeDSNinfo(ConnInfo *ci);

#endif
