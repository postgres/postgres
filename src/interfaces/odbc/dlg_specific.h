/* File:			dlg_specific.h
 *
 * Description:		See "dlg_specific.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __DLG_SPECIFIC_H__
#define __DLG_SPECIFIC_H__

#include "psqlodbc.h"
#include "connection.h"

#ifdef WIN32
#include  <windowsx.h>
#include "resource.h"
#endif

/*	Unknown data type sizes */
#define UNKNOWNS_AS_MAX					0
#define UNKNOWNS_AS_DONTKNOW			1
#define UNKNOWNS_AS_LONGEST				2

/* ODBC initialization files */
#ifndef WIN32
#define ODBC_INI						".odbc.ini"
#define ODBCINST_INI					"odbcinst.ini"
#else
#define ODBC_INI						"ODBC.INI"
#define ODBCINST_INI					"ODBCINST.INI"
#endif


#define INI_DSN							DBMS_NAME		/* Name of default
														 * Datasource in ini
														 * file (not used?) */
#define INI_KDESC						"Description"	/* Data source
														 * description */
#define INI_SERVER						"Servername"	/* Name of Server
														 * running the Postgres
														 * service */
#define INI_PORT						"Port"	/* Port on which the
												 * Postmaster is listening */
#define INI_DATABASE					"Database"		/* Database Name */
#define INI_USER						"Username"		/* Default User Name */
#define INI_PASSWORD					"Password"		/* Default Password */
#define INI_DEBUG						"Debug" /* Debug flag */
#define INI_FETCH						"Fetch" /* Fetch Max Count */
#define INI_SOCKET						"Socket"		/* Socket buffer size */
#define INI_READONLY					"ReadOnly"		/* Database is read only */
#define INI_COMMLOG						"CommLog"		/* Communication to
														 * backend logging */
#define INI_PROTOCOL					"Protocol"		/* What protocol (6.2) */
#define INI_OPTIMIZER					"Optimizer"		/* Use backend genetic
														 * optimizer */
#define INI_KSQO						"Ksqo"	/* Keyset query
												 * optimization */
#define INI_CONNSETTINGS				 "ConnSettings" /* Anything to send to
														 * backend on successful
														 * connection */
#define INI_UNIQUEINDEX					"UniqueIndex"	/* Recognize unique
														 * indexes */
#define INI_UNKNOWNSIZES				"UnknownSizes"	/* How to handle unknown
														 * result set sizes */

#define INI_CANCELASFREESTMT			"CancelAsFreeStmt"

#define INI_USEDECLAREFETCH				"UseDeclareFetch"		/* Use Declare/Fetch
																 * cursors */

/*	More ini stuff */
#define INI_TEXTASLONGVARCHAR			"TextAsLongVarchar"
#define INI_UNKNOWNSASLONGVARCHAR		"UnknownsAsLongVarchar"
#define INI_BOOLSASCHAR					"BoolsAsChar"
#define INI_MAXVARCHARSIZE				"MaxVarcharSize"
#define INI_MAXLONGVARCHARSIZE			"MaxLongVarcharSize"

#define INI_FAKEOIDINDEX				"FakeOidIndex"
#define INI_SHOWOIDCOLUMN				"ShowOidColumn"
#define INI_ROWVERSIONING				"RowVersioning"
#define INI_SHOWSYSTEMTABLES			"ShowSystemTables"
#define INI_LIE							"Lie"
#define INI_PARSE						"Parse"
#define INI_EXTRASYSTABLEPREFIXES		"ExtraSysTablePrefixes"

#define INI_TRANSLATIONNAME				"TranslationName"
#define INI_TRANSLATIONDLL				"TranslationDLL"
#define INI_TRANSLATIONOPTION			"TranslationOption"
#define INI_DISALLOWPREMATURE			"DisallowPremature"
#define INI_UPDATABLECURSORS			"UpdatableCursors"
#define INI_LFCONVERSION			"LFConversion"
#define INI_TRUEISMINUS1			"TrueIsMinus1"
/* Bit representaion for abbreviated connection strings */
#define BIT_LFCONVERSION			(1L)
#define BIT_UPDATABLECURSORS			(1L<<1)
#define BIT_DISALLOWPREMATURE			(1L<<2)
#define BIT_UNIQUEINDEX				(1L<<3)
#define BIT_PROTOCOL_63				(1L<<4)
#define BIT_PROTOCOL_64				(1L<<5)
#define BIT_UNKNOWN_DONTKNOW			(1L<<6)
#define BIT_UNKNOWN_ASMAX			(1L<<7)
#define BIT_OPTIMIZER				(1L<<8)
#define BIT_KSQO				(1L<<9)
#define BIT_COMMLOG				(1L<<10)
#define BIT_DEBUG				(1L<<11)
#define BIT_PARSE				(1L<<12)
#define BIT_CANCELASFREESTMT			(1L<<13)
#define BIT_USEDECLAREFETCH			(1L<<14)
#define BIT_READONLY				(1L<<15)
#define BIT_TEXTASLONGVARCHAR			(1L<<16)
#define BIT_UNKNOWNSASLONGVARCHAR		(1L<<17)
#define BIT_BOOLSASCHAR				(1L<<18)
#define BIT_ROWVERSIONING			(1L<<19)
#define BIT_SHOWSYSTEMTABLES			(1L<<20)
#define BIT_SHOWOIDCOLUMN			(1L<<21)
#define BIT_FAKEOIDINDEX			(1L<<22)
#define BIT_TRUEISMINUS1			(1L<<23)

#define EFFECTIVE_BIT_COUNT			24


/*	Connection Defaults */
#define DEFAULT_PORT					"5432"
#define DEFAULT_READONLY				0
#define DEFAULT_PROTOCOL				"6.4"	/* the latest protocol is
												 * the default */
#define DEFAULT_USEDECLAREFETCH			0
#define DEFAULT_TEXTASLONGVARCHAR		1
#define DEFAULT_UNKNOWNSASLONGVARCHAR	0
#define DEFAULT_BOOLSASCHAR				1
#define DEFAULT_OPTIMIZER				1		/* disable */
#define DEFAULT_KSQO					1		/* on */
#define DEFAULT_UNIQUEINDEX				1		/* dont recognize */
#define DEFAULT_COMMLOG					0		/* dont log */
#define DEFAULT_DEBUG					0
#define DEFAULT_UNKNOWNSIZES			UNKNOWNS_AS_MAX


#define DEFAULT_FAKEOIDINDEX			0
#define DEFAULT_SHOWOIDCOLUMN			0
#define DEFAULT_ROWVERSIONING			0
#define DEFAULT_SHOWSYSTEMTABLES		0		/* dont show system tables */
#define DEFAULT_LIE						0
#define DEFAULT_PARSE					0

#define DEFAULT_CANCELASFREESTMT		0

#define DEFAULT_EXTRASYSTABLEPREFIXES	"dd_;"

#define DEFAULT_DISALLOWPREMATURE	0
#define DEFAULT_TRUEISMINUS1		0
#ifdef	DRIVER_CURSOR_IMPLEMENT
#define DEFAULT_UPDATABLECURSORS	1
#else
#define DEFAULT_UPDATABLECURSORS	0
#endif /* DRIVER_CURSOR_IMPLEMENT */
#ifdef	WIN32
#define DEFAULT_LFCONVERSION		1
#else
#define DEFAULT_LFCONVERSION		0
#endif	/* WIN32 */

/*	prototypes */
void		getCommonDefaults(const char *section, const char *filename, ConnInfo *ci);

#ifdef WIN32
void		SetDlgStuff(HWND hdlg, const ConnInfo *ci);
void		GetDlgStuff(HWND hdlg, ConnInfo *ci);

int CALLBACK driver_optionsProc(HWND hdlg,
				   UINT wMsg,
				   WPARAM wParam,
				   LPARAM lParam);
int CALLBACK ds_optionsProc(HWND hdlg,
			   UINT wMsg,
			   WPARAM wParam,
			   LPARAM lParam);
#endif   /* WIN32 */

void		updateGlobals(void);
void		writeDSNinfo(const ConnInfo *ci);
void		getDSNdefaults(ConnInfo *ci);
void		getDSNinfo(ConnInfo *ci, char overwrite);
void		makeConnectString(char *connect_string, const ConnInfo *ci, UWORD);
void		copyAttributes(ConnInfo *ci, const char *attribute, const char *value);
void		copyCommonAttributes(ConnInfo *ci, const char *attribute, const char *value);

#endif
