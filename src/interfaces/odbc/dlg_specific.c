
/* Module:          dlg_specific.c
 *
 * Description:     This module contains any specific code for handling
 *                  dialog boxes such as driver/datasource options.  Both the
 *                  ConfigDSN() and the SQLDriverConnect() functions use 
 *                  functions in this module.  If you were to add a new option
 *                  to any dialog box, you would most likely only have to change
 *                  things in here rather than in 2 separate places as before.
 *
 * Classes:         none
 *
 * API functions:   none
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef UNIX
#include <string.h>
#include "gpps.h"
#define SQLGetPrivateProfileString(a,b,c,d,e,f) GetPrivateProfileString(a,b,c,d,e,f)
#define SQLWritePrivateProfileString(a,b,c,d) WritePrivateProfileString(a,b,c,d)
#if !HAVE_STRICMP
#define stricmp(s1,s2)	strcasecmp(s1,s2)
#define strnicmp(s1,s2,n)	strncasecmp(s1,s2,n)
#endif
#endif

#include "dlg_specific.h"
#include "convert.h"

extern GLOBAL_VALUES globals;

#ifndef UNIX	/* best to find a #ifdef for WINDOWS */
void
SetDlgStuff(HWND hdlg, ConnInfo *ci)
{
	/*	If driver attribute NOT present, then set the datasource name and description */
	if (ci->driver[0] == '\0') {
		SetDlgItemText(hdlg, IDC_DSNAME, ci->dsn);
		SetDlgItemText(hdlg, IDC_DESC, ci->desc);
	}

	SetDlgItemText(hdlg, IDC_DATABASE, ci->database);
	SetDlgItemText(hdlg, IDC_SERVER, ci->server);
	SetDlgItemText(hdlg, IDC_USER, ci->username);
	SetDlgItemText(hdlg, IDC_PASSWORD, ci->password);
	SetDlgItemText(hdlg, IDC_PORT, ci->port);
}

void 
GetDlgStuff(HWND hdlg, ConnInfo *ci)
{
	GetDlgItemText(hdlg, IDC_DESC, ci->desc, sizeof(ci->desc));

	GetDlgItemText(hdlg, IDC_DATABASE, ci->database, sizeof(ci->database));
	GetDlgItemText(hdlg, IDC_SERVER, ci->server, sizeof(ci->server));
	GetDlgItemText(hdlg, IDC_USER, ci->username, sizeof(ci->username));
	GetDlgItemText(hdlg, IDC_PASSWORD, ci->password, sizeof(ci->password));
	GetDlgItemText(hdlg, IDC_PORT, ci->port, sizeof(ci->port));
}



int CALLBACK driver_optionsProc(HWND   hdlg,
                           WORD   wMsg,
                           WPARAM wParam,
                           LPARAM lParam)
{
	switch (wMsg) {
	case WM_INITDIALOG:

		CheckDlgButton(hdlg, DRV_COMMLOG, globals.commlog);
		CheckDlgButton(hdlg, DRV_OPTIMIZER, globals.disable_optimizer);
		CheckDlgButton(hdlg, DRV_UNIQUEINDEX, globals.unique_index);
		CheckDlgButton(hdlg, DRV_READONLY, globals.readonly);
		CheckDlgButton(hdlg, DRV_USEDECLAREFETCH, globals.use_declarefetch);

		/*	Unknown (Default) Data Type sizes */
		switch(globals.unknown_sizes) {
		case UNKNOWNS_AS_DONTKNOW:
			CheckDlgButton(hdlg, DRV_UNKNOWN_DONTKNOW, 1);
			break;
		case UNKNOWNS_AS_LONGEST:
			CheckDlgButton(hdlg, DRV_UNKNOWN_LONGEST, 1);
			break;
		case UNKNOWNS_AS_MAX:
		default:
			CheckDlgButton(hdlg, DRV_UNKNOWN_MAX, 1);
			break;
		}

		CheckDlgButton(hdlg, DRV_TEXT_LONGVARCHAR, globals.text_as_longvarchar);
		CheckDlgButton(hdlg, DRV_UNKNOWNS_LONGVARCHAR, globals.unknowns_as_longvarchar);
		CheckDlgButton(hdlg, DRV_BOOLS_CHAR, globals.bools_as_char);

		CheckDlgButton(hdlg, DRV_PARSE, globals.parse);

		SetDlgItemInt(hdlg, DRV_CACHE_SIZE, globals.fetch_max, FALSE);
		SetDlgItemInt(hdlg, DRV_VARCHAR_SIZE, globals.max_varchar_size, FALSE);
		SetDlgItemInt(hdlg, DRV_LONGVARCHAR_SIZE, globals.max_longvarchar_size, TRUE);

		SetDlgItemText(hdlg, DRV_EXTRASYSTABLEPREFIXES, globals.extra_systable_prefixes);

		/*	Driver Connection Settings */
		SetDlgItemText(hdlg, DRV_CONNSETTINGS, globals.conn_settings);

		break; 

	case WM_COMMAND:
		switch (GET_WM_COMMAND_ID(wParam, lParam)) {
		case IDOK:

			globals.commlog = IsDlgButtonChecked(hdlg, DRV_COMMLOG);
			globals.disable_optimizer = IsDlgButtonChecked(hdlg, DRV_OPTIMIZER);
			globals.unique_index = IsDlgButtonChecked(hdlg, DRV_UNIQUEINDEX);
			globals.readonly = IsDlgButtonChecked(hdlg, DRV_READONLY);
			globals.use_declarefetch = IsDlgButtonChecked(hdlg, DRV_USEDECLAREFETCH);

			/*	Unknown (Default) Data Type sizes */
			if (IsDlgButtonChecked(hdlg, DRV_UNKNOWN_MAX))
				globals.unknown_sizes = UNKNOWNS_AS_MAX;
			else if (IsDlgButtonChecked(hdlg, DRV_UNKNOWN_DONTKNOW))
				globals.unknown_sizes = UNKNOWNS_AS_DONTKNOW;
			else if (IsDlgButtonChecked(hdlg, DRV_UNKNOWN_LONGEST))
				globals.unknown_sizes = UNKNOWNS_AS_LONGEST;
			else
				globals.unknown_sizes = UNKNOWNS_AS_MAX;

			globals.text_as_longvarchar = IsDlgButtonChecked(hdlg, DRV_TEXT_LONGVARCHAR);
			globals.unknowns_as_longvarchar = IsDlgButtonChecked(hdlg, DRV_UNKNOWNS_LONGVARCHAR);
			globals.bools_as_char = IsDlgButtonChecked(hdlg, DRV_BOOLS_CHAR);

			globals.parse = IsDlgButtonChecked(hdlg, DRV_PARSE);

			globals.fetch_max = GetDlgItemInt(hdlg, DRV_CACHE_SIZE, NULL, FALSE);
			globals.max_varchar_size = GetDlgItemInt(hdlg, DRV_VARCHAR_SIZE, NULL, FALSE);
			globals.max_longvarchar_size= GetDlgItemInt(hdlg, DRV_LONGVARCHAR_SIZE, NULL, TRUE);	// allows for SQL_NO_TOTAL

			GetDlgItemText(hdlg, DRV_EXTRASYSTABLEPREFIXES, globals.extra_systable_prefixes, sizeof(globals.extra_systable_prefixes));

			/*	Driver Connection Settings */
			GetDlgItemText(hdlg, DRV_CONNSETTINGS, globals.conn_settings, sizeof(globals.conn_settings));

			updateGlobals();

			//	fall through

		case IDCANCEL:
			EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
			return TRUE;

		case IDDEFAULTS:
			CheckDlgButton(hdlg, DRV_COMMLOG, DEFAULT_COMMLOG);
			CheckDlgButton(hdlg, DRV_OPTIMIZER, DEFAULT_OPTIMIZER);
			CheckDlgButton(hdlg, DRV_UNIQUEINDEX, DEFAULT_UNIQUEINDEX);
			CheckDlgButton(hdlg, DRV_READONLY, DEFAULT_READONLY);
			CheckDlgButton(hdlg, DRV_USEDECLAREFETCH, DEFAULT_USEDECLAREFETCH);
	
			CheckDlgButton(hdlg, DRV_PARSE, DEFAULT_PARSE);

			/*	Unknown Sizes */
			CheckDlgButton(hdlg, DRV_UNKNOWN_DONTKNOW, 0);
			CheckDlgButton(hdlg, DRV_UNKNOWN_LONGEST, 0);
			CheckDlgButton(hdlg, DRV_UNKNOWN_MAX, 0);
			switch(DEFAULT_UNKNOWNSIZES) {
			case UNKNOWNS_AS_DONTKNOW:
				CheckDlgButton(hdlg, DRV_UNKNOWN_DONTKNOW, 1);
				break;
			case UNKNOWNS_AS_LONGEST:
				CheckDlgButton(hdlg, DRV_UNKNOWN_LONGEST, 1);
				break;
			case UNKNOWNS_AS_MAX:
				CheckDlgButton(hdlg, DRV_UNKNOWN_MAX, 1);
				break;
			}

			CheckDlgButton(hdlg, DRV_TEXT_LONGVARCHAR, DEFAULT_TEXTASLONGVARCHAR);
			CheckDlgButton(hdlg, DRV_UNKNOWNS_LONGVARCHAR, DEFAULT_UNKNOWNSASLONGVARCHAR);
			CheckDlgButton(hdlg, DRV_BOOLS_CHAR, DEFAULT_BOOLSASCHAR);

			SetDlgItemInt(hdlg, DRV_CACHE_SIZE, FETCH_MAX, FALSE);
			SetDlgItemInt(hdlg, DRV_VARCHAR_SIZE, MAX_VARCHAR_SIZE, FALSE);
			SetDlgItemInt(hdlg, DRV_LONGVARCHAR_SIZE, TEXT_FIELD_SIZE, TRUE);

			SetDlgItemText(hdlg, DRV_EXTRASYSTABLEPREFIXES, DEFAULT_EXTRASYSTABLEPREFIXES);

			/*	Driver Connection Settings */
			SetDlgItemText(hdlg, DRV_CONNSETTINGS, "");

			break;
		}

	}

	return FALSE;
}

int CALLBACK ds_optionsProc(HWND   hdlg,
                           WORD   wMsg,
                           WPARAM wParam,
                           LPARAM lParam)
{
ConnInfo *ci;
char buf[128];

	switch (wMsg) {
	case WM_INITDIALOG:
		ci = (ConnInfo *) lParam;		
		SetWindowLong(hdlg, DWL_USER, lParam);	// save for OK

		/*	Change window caption */
		if (ci->driver[0])
			SetWindowText(hdlg, "Advanced Options (Connection)");
		else {
			sprintf(buf, "Advanced Options (%s)", ci->dsn);
			SetWindowText(hdlg, buf);
		}

		/*	Readonly */
		CheckDlgButton(hdlg, DS_READONLY, atoi(ci->readonly));

		/*	Protocol */
		if (strncmp(ci->protocol, PG62, strlen(PG62)) == 0)
			CheckDlgButton(hdlg, DS_PG62, 1);
		else
			CheckDlgButton(hdlg, DS_PG62, 0);


		CheckDlgButton(hdlg, DS_SHOWOIDCOLUMN, atoi(ci->show_oid_column));
		CheckDlgButton(hdlg, DS_FAKEOIDINDEX, atoi(ci->fake_oid_index));
		CheckDlgButton(hdlg, DS_ROWVERSIONING, atoi(ci->row_versioning));
		CheckDlgButton(hdlg, DS_SHOWSYSTEMTABLES, atoi(ci->show_system_tables));

		EnableWindow(GetDlgItem(hdlg, DS_FAKEOIDINDEX), atoi(ci->show_oid_column));

		/*	Datasource Connection Settings */
		SetDlgItemText(hdlg, DS_CONNSETTINGS, ci->conn_settings);
		break; 


	case WM_COMMAND:
		switch (GET_WM_COMMAND_ID(wParam, lParam)) {
		case DS_SHOWOIDCOLUMN:
			mylog("WM_COMMAND: DS_SHOWOIDCOLUMN\n");
			EnableWindow(GetDlgItem(hdlg, DS_FAKEOIDINDEX), IsDlgButtonChecked(hdlg, DS_SHOWOIDCOLUMN));
			return TRUE;


		case IDOK: 

			ci = (ConnInfo *)GetWindowLong(hdlg, DWL_USER);
			mylog("IDOK: got ci = %u\n", ci);

			/*	Readonly */
			sprintf(ci->readonly, "%d", IsDlgButtonChecked(hdlg, DS_READONLY));

			/*	Protocol */
			if ( IsDlgButtonChecked(hdlg, DS_PG62))
				strcpy(ci->protocol, PG62);
			else 
				ci->protocol[0] = '\0';



			sprintf(ci->show_system_tables, "%d", IsDlgButtonChecked(hdlg, DS_SHOWSYSTEMTABLES));

			sprintf(ci->row_versioning, "%d", IsDlgButtonChecked(hdlg, DS_ROWVERSIONING));

			/*	OID Options*/
			sprintf(ci->fake_oid_index, "%d", IsDlgButtonChecked(hdlg, DS_FAKEOIDINDEX));
			sprintf(ci->show_oid_column, "%d", IsDlgButtonChecked(hdlg, DS_SHOWOIDCOLUMN));

			/*	Datasource Connection Settings */
			GetDlgItemText(hdlg, DS_CONNSETTINGS, ci->conn_settings, sizeof(ci->conn_settings));


			//	fall through

		case IDCANCEL:
			EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
			return TRUE;
		}
	}

	return FALSE;
}

#endif	/* !UNIX */

void
makeConnectString(char *connect_string, ConnInfo *ci)
{
char got_dsn = (ci->dsn[0] != '\0');
char encoded_conn_settings[LARGE_REGISTRY_LEN];

	/*	fundamental info */
	sprintf(connect_string, "%s=%s;DATABASE=%s;SERVER=%s;PORT=%s;UID=%s;PWD=%s",
		got_dsn ? "DSN" : "DRIVER", 
		got_dsn ? ci->dsn : ci->driver,
		ci->database,
		ci->server,
		ci->port,
		ci->username,
		ci->password);

	encode(ci->conn_settings, encoded_conn_settings);

	/*	extra info */
	sprintf(&connect_string[strlen(connect_string)], 
		";READONLY=%s;PROTOCOL=%s;FAKEOIDINDEX=%s;SHOWOIDCOLUMN=%s;ROWVERSIONING=%s;SHOWSYSTEMTABLES=%s;CONNSETTINGS=%s", 
		ci->readonly,
		ci->protocol,
		ci->fake_oid_index,
		ci->show_oid_column,
		ci->row_versioning,
		ci->show_system_tables,
		encoded_conn_settings);
}

void
copyAttributes(ConnInfo *ci, char *attribute, char *value)
{

	if(stricmp(attribute, "DSN") == 0)
		strcpy(ci->dsn, value);

	else if(stricmp(attribute, "driver") == 0)
		strcpy(ci->driver, value);

	else if(stricmp(attribute, INI_DATABASE) == 0)
		strcpy(ci->database, value);

	else if(stricmp(attribute, INI_SERVER) == 0 || stricmp(attribute, "server") == 0)
		strcpy(ci->server, value);

	else if(stricmp(attribute, INI_USER) == 0 || stricmp(attribute, "uid") == 0)
		strcpy(ci->username, value);

	else if(stricmp(attribute, INI_PASSWORD) == 0 || stricmp(attribute, "pwd") == 0)
		strcpy(ci->password, value);

	else if(stricmp(attribute, INI_PORT) == 0)
		strcpy(ci->port, value);

	else if (stricmp(attribute, INI_READONLY) == 0)
		strcpy(ci->readonly, value);

	else if (stricmp(attribute, INI_PROTOCOL) == 0)
		strcpy(ci->protocol, value);

	else if (stricmp(attribute, INI_SHOWOIDCOLUMN) == 0)
		strcpy(ci->show_oid_column, value);

	else if (stricmp(attribute, INI_FAKEOIDINDEX) == 0)
		strcpy(ci->fake_oid_index, value);

	else if (stricmp(attribute, INI_ROWVERSIONING) == 0)
		strcpy(ci->row_versioning, value);

	else if (stricmp(attribute, INI_SHOWSYSTEMTABLES) == 0)
		strcpy(ci->show_system_tables, value);

	else if (stricmp(attribute, INI_CONNSETTINGS) == 0) {
		decode(value, ci->conn_settings);
		// strcpy(ci->conn_settings, value);
	}

	mylog("copyAttributes: DSN='%s',server='%s',dbase='%s',user='%s',passwd='%s',port='%s',readonly='%s',protocol='%s', conn_settings='%s')\n", 
		ci->dsn, 
		ci->server,
		ci->database,
		ci->username,
		ci->password,
		ci->port,
		ci->readonly,
		ci->protocol,
		ci->conn_settings);

}

void
getDSNdefaults(ConnInfo *ci)
{
	if (ci->port[0] == '\0')
		strcpy(ci->port, DEFAULT_PORT);

	if (ci->readonly[0] == '\0')
		sprintf(ci->readonly, "%d", globals.readonly);

	if (ci->fake_oid_index[0] == '\0')
		sprintf(ci->fake_oid_index, "%d", DEFAULT_FAKEOIDINDEX);

	if (ci->show_oid_column[0] == '\0')
		sprintf(ci->show_oid_column, "%d", DEFAULT_SHOWOIDCOLUMN);

	if (ci->show_system_tables[0] == '\0')
		sprintf(ci->show_system_tables, "%d", DEFAULT_SHOWSYSTEMTABLES);

	if (ci->row_versioning[0] == '\0')
		sprintf(ci->row_versioning, "%d", DEFAULT_ROWVERSIONING);
}


void 
getDSNinfo(ConnInfo *ci, char overwrite)
{
char *DSN = ci->dsn;
char encoded_conn_settings[LARGE_REGISTRY_LEN];

	//	If a driver keyword was present, then dont use a DSN and return.
	//	If DSN is null and no driver, then use the default datasource.
	if ( DSN[0] == '\0') {
		if ( ci->driver[0] != '\0')
			return;
		else
			strcpy(DSN, INI_DSN);
	}

	//	Proceed with getting info for the given DSN.

	if ( ci->desc[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_KDESC, "", ci->desc, sizeof(ci->desc), ODBC_INI);

	if ( ci->server[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_SERVER, "", ci->server, sizeof(ci->server), ODBC_INI);

	if ( ci->database[0] == '\0' || overwrite)
	    SQLGetPrivateProfileString(DSN, INI_DATABASE, "", ci->database, sizeof(ci->database), ODBC_INI);

	if ( ci->username[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_USER, "", ci->username, sizeof(ci->username), ODBC_INI);

	if ( ci->password[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_PASSWORD, "", ci->password, sizeof(ci->password), ODBC_INI);

	if ( ci->port[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_PORT, "", ci->port, sizeof(ci->port), ODBC_INI);

	if ( ci->readonly[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_READONLY, "", ci->readonly, sizeof(ci->readonly), ODBC_INI);

	if ( ci->show_oid_column[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_SHOWOIDCOLUMN, "", ci->show_oid_column, sizeof(ci->show_oid_column), ODBC_INI);

	if ( ci->fake_oid_index[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_FAKEOIDINDEX, "", ci->fake_oid_index, sizeof(ci->fake_oid_index), ODBC_INI);

	if ( ci->row_versioning[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_ROWVERSIONING, "", ci->row_versioning, sizeof(ci->row_versioning), ODBC_INI);

	if ( ci->show_system_tables[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_SHOWSYSTEMTABLES, "", ci->show_system_tables, sizeof(ci->show_system_tables), ODBC_INI);

	if ( ci->protocol[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_PROTOCOL, "", ci->protocol, sizeof(ci->protocol), ODBC_INI);

	if ( ci->conn_settings[0] == '\0' || overwrite) {
		SQLGetPrivateProfileString(DSN, INI_CONNSETTINGS, "", encoded_conn_settings, sizeof(encoded_conn_settings), ODBC_INI);
		decode(encoded_conn_settings, ci->conn_settings);
	}

	if ( ci->translation_dll[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_TRANSLATIONDLL, "", ci->translation_dll, sizeof(ci->translation_dll), ODBC_INI);

	if ( ci->translation_option[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_TRANSLATIONOPTION, "", ci->translation_option, sizeof(ci->translation_option), ODBC_INI);

	qlog("DSN info: DSN='%s',server='%s',port='%s',dbase='%s',user='%s',passwd='%s'\n", 
		DSN, 
		ci->server,
		ci->port,
		ci->database,
		ci->username,
		ci->password);
	qlog("          readonly='%s',protocol='%s',showoid='%s',fakeoidindex='%s',showsystable='%s'\n",
		ci->readonly,
		ci->protocol,
		ci->show_oid_column,
		ci->fake_oid_index,
		ci->show_system_tables);
	qlog("          conn_settings='%s'\n",
		ci->conn_settings);
	qlog("          translation_dll='%s',translation_option='%s'\n",
		ci->translation_dll,
		ci->translation_option);

}


/*	This is for datasource based options only */
void
writeDSNinfo(ConnInfo *ci)
{
char *DSN = ci->dsn;
char encoded_conn_settings[LARGE_REGISTRY_LEN];

		encode(ci->conn_settings, encoded_conn_settings);

		SQLWritePrivateProfileString(DSN,
			INI_KDESC,
			ci->desc,
			ODBC_INI);
                        
		SQLWritePrivateProfileString(DSN,
			INI_DATABASE,
			ci->database,
			ODBC_INI);
                        
		SQLWritePrivateProfileString(DSN,
			INI_SERVER,
			ci->server,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_PORT,
			ci->port,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_USER,
			ci->username,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_PASSWORD,
			ci->password,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_READONLY,
			ci->readonly,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_SHOWOIDCOLUMN,
			ci->show_oid_column,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_FAKEOIDINDEX,
			ci->fake_oid_index,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_ROWVERSIONING,
			ci->row_versioning,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_SHOWSYSTEMTABLES,
			ci->show_system_tables,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_PROTOCOL,
			ci->protocol,
			ODBC_INI);

		SQLWritePrivateProfileString(DSN,
			INI_CONNSETTINGS,
			encoded_conn_settings,
			ODBC_INI);
}


/*	This function reads the ODBCINST.INI portion of
	the registry and gets any driver defaults.
*/
void getGlobalDefaults(void)
{
char temp[128];


	//	Fetch Count is stored in driver section
    SQLGetPrivateProfileString(DBMS_NAME, INI_FETCH, "",
                            temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] ) {
		globals.fetch_max = atoi(temp);
		/*	sanity check if using cursors */
		if (globals.fetch_max <= 0)
			globals.fetch_max = FETCH_MAX;
	}

	else
		globals.fetch_max = FETCH_MAX;


	//	Socket Buffersize is stored in driver section
    SQLGetPrivateProfileString(DBMS_NAME, INI_SOCKET, "",
                            temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] ) 
		globals.socket_buffersize = atoi(temp);
	else
		globals.socket_buffersize = SOCK_BUFFER_SIZE;


	//	Debug is stored in the driver section
	SQLGetPrivateProfileString(DBMS_NAME, INI_DEBUG, "0", 
							temp, sizeof(temp), ODBCINST_INI);
	globals.debug = atoi(temp);


	//	CommLog is stored in the driver section
	SQLGetPrivateProfileString(DBMS_NAME, INI_COMMLOG, "", 
							temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.commlog = DEFAULT_COMMLOG;
	else
		globals.commlog = atoi(temp);


	//	Optimizer is stored in the driver section only
	SQLGetPrivateProfileString(DBMS_NAME, INI_OPTIMIZER, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.disable_optimizer = DEFAULT_OPTIMIZER;
	else
		globals.disable_optimizer = atoi(temp);


	//	Recognize Unique Index is stored in the driver section only
	SQLGetPrivateProfileString(DBMS_NAME, INI_UNIQUEINDEX, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.unique_index = DEFAULT_UNIQUEINDEX;
	else
		globals.unique_index = atoi(temp);


	//	Unknown Sizes is stored in the driver section only
	SQLGetPrivateProfileString(DBMS_NAME, INI_UNKNOWNSIZES, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0')
		globals.unknown_sizes = DEFAULT_UNKNOWNSIZES;
	else
		globals.unknown_sizes = atoi(temp);


	//	Lie about supported functions?
	SQLGetPrivateProfileString(DBMS_NAME, INI_LIE, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.lie = DEFAULT_LIE;
	else
		globals.lie = atoi(temp);

	//	Parse statements
	SQLGetPrivateProfileString(DBMS_NAME, INI_PARSE, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.parse = DEFAULT_PARSE;
	else
		globals.parse = atoi(temp);

	//	Readonly is stored in the driver section AND per datasource
	SQLGetPrivateProfileString(DBMS_NAME, INI_READONLY, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.readonly = DEFAULT_READONLY;
	else
		globals.readonly = atoi(temp);


	//	UseDeclareFetch is stored in the driver section only
	SQLGetPrivateProfileString(DBMS_NAME, INI_USEDECLAREFETCH, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.use_declarefetch = DEFAULT_USEDECLAREFETCH;
	else
		globals.use_declarefetch = atoi(temp);


	//	Max Varchar Size
	SQLGetPrivateProfileString(DBMS_NAME, INI_MAXVARCHARSIZE, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.max_varchar_size = MAX_VARCHAR_SIZE;
	else
		globals.max_varchar_size = atoi(temp);

	//	Max TextField Size
	SQLGetPrivateProfileString(DBMS_NAME, INI_MAXLONGVARCHARSIZE, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.max_longvarchar_size = TEXT_FIELD_SIZE;
	else
		globals.max_longvarchar_size = atoi(temp);

	//	Text As LongVarchar 
	SQLGetPrivateProfileString(DBMS_NAME, INI_TEXTASLONGVARCHAR, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.text_as_longvarchar = DEFAULT_TEXTASLONGVARCHAR;
	else
		globals.text_as_longvarchar = atoi(temp);

	//	Unknowns As LongVarchar 
	SQLGetPrivateProfileString(DBMS_NAME, INI_UNKNOWNSASLONGVARCHAR, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.unknowns_as_longvarchar = DEFAULT_UNKNOWNSASLONGVARCHAR;
	else
		globals.unknowns_as_longvarchar = atoi(temp);

	//	Bools As Char
	SQLGetPrivateProfileString(DBMS_NAME, INI_BOOLSASCHAR, "", 
				temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] == '\0') 
		globals.bools_as_char = DEFAULT_BOOLSASCHAR;
	else
		globals.bools_as_char = atoi(temp);


	//	Extra System Table prefixes
	SQLGetPrivateProfileString(DBMS_NAME, INI_EXTRASYSTABLEPREFIXES, "@@@", 
			globals.extra_systable_prefixes, sizeof(globals.extra_systable_prefixes), ODBCINST_INI);
	if ( ! strcmp(globals.extra_systable_prefixes, "@@@")) {
		strcpy(globals.extra_systable_prefixes, DEFAULT_EXTRASYSTABLEPREFIXES);
	}
	mylog("globals.extra_systable_prefixes = '%s'\n", globals.extra_systable_prefixes);


	//	ConnSettings is stored in the driver section and per datasource for override
	SQLGetPrivateProfileString(DBMS_NAME, INI_CONNSETTINGS, "", 
				globals.conn_settings, sizeof(globals.conn_settings), ODBCINST_INI);


}


/*	This function writes any global parameters (that can be manipulated)
	to the ODBCINST.INI portion of the registry 
*/
void updateGlobals(void)
{
char tmp[128];

	sprintf(tmp, "%d", globals.fetch_max);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_FETCH, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.commlog);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_COMMLOG, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.disable_optimizer);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_OPTIMIZER, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.unique_index);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_UNIQUEINDEX, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.readonly);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_READONLY, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.use_declarefetch);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_USEDECLAREFETCH, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.unknown_sizes);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_UNKNOWNSIZES, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.text_as_longvarchar);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_TEXTASLONGVARCHAR, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.unknowns_as_longvarchar);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_UNKNOWNSASLONGVARCHAR, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.bools_as_char);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_BOOLSASCHAR, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.parse);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_PARSE, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.max_varchar_size);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_MAXVARCHARSIZE, tmp, ODBCINST_INI);

	sprintf(tmp, "%d", globals.max_longvarchar_size);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_MAXLONGVARCHARSIZE, tmp, ODBCINST_INI);

	SQLWritePrivateProfileString(DBMS_NAME,
		INI_EXTRASYSTABLEPREFIXES, globals.extra_systable_prefixes, ODBCINST_INI);

	SQLWritePrivateProfileString(DBMS_NAME,
		INI_CONNSETTINGS, globals.conn_settings, ODBCINST_INI);
}
