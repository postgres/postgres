
/* Module:          drvconn.c
 *
 * Description:     This module contains only routines related to 
 *                  implementing SQLDriverConnect.
 *
 * Classes:         n/a
 *
 * API functions:   SQLDriverConnect
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include "psqlodbc.h"
#include "connection.h"

#include <winsock.h>
#include <sqlext.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>
#include <odbcinst.h>
#include "resource.h"

/* prototypes */
BOOL FAR PASCAL dconn_FDriverConnectProc(HWND hdlg, UINT wMsg, WPARAM wParam, LPARAM lParam);
RETCODE dconn_DoDialog(HWND hwnd, ConnInfo *ci);
void dconn_get_connect_attributes(UCHAR FAR *connect_string, ConnInfo *ci);


extern HINSTANCE NEAR s_hModule;               /* Saved module handle. */
extern GLOBAL_VALUES globals;

RETCODE SQL_API SQLDriverConnect(
                                 HDBC      hdbc,
                                 HWND      hwnd,
                                 UCHAR FAR *szConnStrIn,
                                 SWORD     cbConnStrIn,
                                 UCHAR FAR *szConnStrOut,
                                 SWORD     cbConnStrOutMax,
                                 SWORD FAR *pcbConnStrOut,
                                 UWORD     fDriverCompletion)
{
ConnectionClass *conn = (ConnectionClass *) hdbc;
ConnInfo *ci;
RETCODE dialog_result;
char connect_string[MAX_CONNECT_STRING];
int retval;
char password_required = FALSE;

	mylog("**** SQLDriverConnect: fDriverCompletion=%d, connStrIn='%s'\n", fDriverCompletion, szConnStrIn);

	if ( ! conn) 
		return SQL_INVALID_HANDLE;

	qlog("conn=%u, SQLDriverConnect( in)='%s'\n", conn, szConnStrIn);

	ci = &(conn->connInfo);

	//	Parse the connect string and fill in conninfo for this hdbc.
	dconn_get_connect_attributes(szConnStrIn, ci);

	//	If the ConnInfo in the hdbc is missing anything,
	//	this function will fill them in from the registry (assuming
	//	of course there is a DSN given -- if not, it does nothing!)
	CC_DSN_info(conn, CONN_DONT_OVERWRITE);

	//	Fill in any default parameters if they are not there.
	CC_set_defaults(conn);

dialog:
	ci->focus_password = password_required;

	switch(fDriverCompletion) {
	case SQL_DRIVER_PROMPT:
		dialog_result = dconn_DoDialog(hwnd, ci);
		if(dialog_result != SQL_SUCCESS) {
			return dialog_result;
		}
		break;

	case SQL_DRIVER_COMPLETE:
	case SQL_DRIVER_COMPLETE_REQUIRED:
	/* Password is not a required parameter. */
		if( ci->username[0] == '\0' ||
			ci->server[0] == '\0' ||
			ci->database[0] == '\0' ||
			ci->port[0] == '\0' ||
			password_required) { 

			dialog_result = dconn_DoDialog(hwnd, ci);
			if(dialog_result != SQL_SUCCESS) {
				return dialog_result;
			}
		}
		break;
	case SQL_DRIVER_NOPROMPT:
		break;
	}

	/*	Password is not a required parameter unless authentication asks for it.
		For now, I think its better to just let the application ask over and over until
		a password is entered (the user can always hit Cancel to get out)
	*/
	if( ci->username[0] == '\0' ||
		ci->server[0] == '\0' ||
		ci->database[0] == '\0' || 
		ci->port[0] == '\0') {
//		(password_required && ci->password[0] == '\0'))

		return SQL_NO_DATA_FOUND;
	}

	if(szConnStrOut) {

		//	return the completed string to the caller.

		char got_dsn = (ci->dsn[0] != '\0');

		sprintf(connect_string, "%s=%s;DATABASE=%s;SERVER=%s;PORT=%s;UID=%s;READONLY=%s;PWD=%s;PROTOCOL=%s;CONNSETTINGS=%s", 
			got_dsn ? "DSN" : "DRIVER", 
			got_dsn ? ci->dsn : ci->driver,
			ci->database,
			ci->server,
			ci->port,
			ci->username,
			ci->readonly,
			ci->password,
			ci->protocol,
			ci->conn_settings);

		if(pcbConnStrOut) {
			*pcbConnStrOut = strlen(connect_string);
		}
		strncpy_null(szConnStrOut, connect_string, cbConnStrOutMax);
	}

	mylog("szConnStrOut = '%s'\n", szConnStrOut);
	qlog("conn=%u, SQLDriverConnect(out)='%s'\n", conn, szConnStrOut);

	// do the actual connect
	retval = CC_connect(conn, password_required);
	if (retval < 0) {		/* need a password */
		if (fDriverCompletion == SQL_DRIVER_NOPROMPT)
			return SQL_ERROR;	/* need a password but not allowed to prompt so error */
		else {
			password_required = TRUE;
			goto dialog;
		}
	}
	else if (retval == 0) {
		//	error msg filled in above
		return SQL_ERROR;
	}

	mylog("SQLDRiverConnect: returning success\n");
	return SQL_SUCCESS;
}


RETCODE dconn_DoDialog(HWND hwnd, ConnInfo *ci)
{
int dialog_result;

mylog("dconn_DoDialog: ci = %u\n", ci);

	if(hwnd) {
		dialog_result = DialogBoxParam(s_hModule, MAKEINTRESOURCE(DRIVERCONNDIALOG),
									hwnd, dconn_FDriverConnectProc, (LPARAM) ci);
		if(!dialog_result || (dialog_result == -1)) {
			return SQL_NO_DATA_FOUND;
		} else {
			return SQL_SUCCESS;
		}
	}

	return SQL_ERROR;
}


BOOL FAR PASCAL dconn_FDriverConnectProc(
                                         HWND    hdlg,
                                         UINT    wMsg,
                                         WPARAM  wParam,
                                         LPARAM  lParam)
{
static ConnInfo *ci;

	switch (wMsg) {
	case WM_INITDIALOG:
		ci = (ConnInfo *) lParam;		// Save the ConnInfo for the "OK"

		SetDlgItemText(hdlg, SERVER_EDIT, ci->server);
		SetDlgItemText(hdlg, DATABASE_EDIT, ci->database);
		SetDlgItemText(hdlg, USERNAME_EDIT, ci->username);
		SetDlgItemText(hdlg, PASSWORD_EDIT, ci->password);
		SetDlgItemText(hdlg, PORT_EDIT, ci->port);
		CheckDlgButton(hdlg, READONLY_EDIT, atoi(ci->readonly));

		CheckDlgButton(hdlg, PG62_EDIT, PROTOCOL_62(ci));

		/*	The driver connect dialog box allows manipulating this global variable */
		CheckDlgButton(hdlg, COMMLOG_EDIT, globals.commlog);

		if (ci->database[0] == '\0')		
			;	/* default focus */
		else if (ci->server[0] == '\0')
			SetFocus(GetDlgItem(hdlg, SERVER_EDIT));
		else if (ci->port[0] == '\0')
			SetFocus(GetDlgItem(hdlg, PORT_EDIT));
		else if (ci->username[0] == '\0')
			SetFocus(GetDlgItem(hdlg, USERNAME_EDIT));
		else if (ci->focus_password)
			SetFocus(GetDlgItem(hdlg, PASSWORD_EDIT));

		break; 

	case WM_COMMAND:
		switch (GET_WM_COMMAND_ID(wParam, lParam)) {
		case IDOK:

			GetDlgItemText(hdlg, SERVER_EDIT, ci->server, sizeof(ci->server));
			GetDlgItemText(hdlg, DATABASE_EDIT, ci->database, sizeof(ci->database));
  			GetDlgItemText(hdlg, USERNAME_EDIT, ci->username, sizeof(ci->username));
			GetDlgItemText(hdlg, PASSWORD_EDIT, ci->password, sizeof(ci->password));
			GetDlgItemText(hdlg, PORT_EDIT, ci->port, sizeof(ci->port));

			sprintf(ci->readonly, "%d", IsDlgButtonChecked(hdlg, READONLY_EDIT));

			if (IsDlgButtonChecked(hdlg, PG62_EDIT))
				strcpy(ci->protocol, PG62);
			else
				ci->protocol[0] = '\0';

			/*	The driver connect dialog box allows manipulating this global variable */
			globals.commlog = IsDlgButtonChecked(hdlg, COMMLOG_EDIT);
			updateGlobals();

		case IDCANCEL:
			EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
			return TRUE;
		}
	}

	return FALSE;
}



void dconn_get_connect_attributes(UCHAR FAR *connect_string, ConnInfo *ci)
{
char *our_connect_string;
char *pair, *attribute, *value, *equals;
char *strtok_arg;

	memset(ci, 0, sizeof(ConnInfo));

	our_connect_string = strdup(connect_string);
	strtok_arg = our_connect_string;

	mylog("our_connect_string = '%s'\n", our_connect_string);

	while(1) {
		pair = strtok(strtok_arg, ";");
		if(strtok_arg) {
			strtok_arg = 0;
		}
		if(!pair) {
			break;
		}

		equals = strchr(pair, '=');
		if ( ! equals)
			continue;

		*equals = '\0';
		attribute = pair;		//	ex. DSN
		value = equals + 1;		//	ex. 'CEO co1'

		mylog("attribute = '%s', value = '%s'\n", attribute, value);

		if( !attribute || !value)
			continue;          

		/*********************************************************/
		/*               PARSE ATTRIBUTES                        */
		/*********************************************************/
		if(stricmp(attribute, "DSN") == 0)
			strcpy(ci->dsn, value);

		else if(stricmp(attribute, "driver") == 0)
			strcpy(ci->driver, value);

		else if(stricmp(attribute, "uid") == 0)
			strcpy(ci->username, value);

		else if(stricmp(attribute, "pwd") == 0)
			strcpy(ci->password, value);

		else if ((stricmp(attribute, "server") == 0) ||
					(stricmp(attribute, "servername") == 0))
			strcpy(ci->server, value);

		else if(stricmp(attribute, "port") == 0)
			strcpy(ci->port, value);

		else if(stricmp(attribute, "database") == 0)
			strcpy(ci->database, value);

		else if (stricmp(attribute, "readonly") == 0)
			strcpy(ci->readonly, value);

		else if (stricmp(attribute, "protocol") == 0)
			strcpy(ci->protocol, value);

		else if (stricmp(attribute, "connsettings") == 0)
			strcpy(ci->conn_settings, value);

	}


	free(our_connect_string);
}
