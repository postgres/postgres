/* Module:			drvconn.c
 *
 * Description:		This module contains only routines related to
 *					implementing SQLDriverConnect.
 *
 * Classes:			n/a
 *
 * API functions:	SQLDriverConnect
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "psqlodbc.h"
#include "connection.h"

#ifndef WIN32
#include <sys/types.h>
#include <sys/socket.h>
#define NEAR
#else
#include <winsock.h>
#include <sqlext.h>
#endif

#include <string.h>

#ifndef WIN32
#define stricmp(s1,s2)	strcasecmp(s1,s2)
#define strnicmp(s1,s2,n)	strncasecmp(s1,s2,n)
#else
#include <windows.h>
#include <windowsx.h>
#include <odbcinst.h>
#include "resource.h"
#endif

#ifndef TRUE
#define TRUE	(BOOL)1
#endif
#ifndef FALSE
#define FALSE	(BOOL)0
#endif

#include "dlg_specific.h"

/* prototypes */
void		dconn_get_connect_attributes(UCHAR FAR *connect_string, ConnInfo *ci);

#ifdef WIN32
BOOL FAR PASCAL dconn_FDriverConnectProc(HWND hdlg, UINT wMsg, WPARAM wParam, LPARAM lParam);
RETCODE		dconn_DoDialog(HWND hwnd, ConnInfo *ci);

extern HINSTANCE NEAR s_hModule;/* Saved module handle. */

#endif

extern GLOBAL_VALUES globals;


RETCODE		SQL_API
SQLDriverConnect(
				 HDBC hdbc,
				 HWND hwnd,
				 UCHAR FAR *szConnStrIn,
				 SWORD cbConnStrIn,
				 UCHAR FAR *szConnStrOut,
				 SWORD cbConnStrOutMax,
				 SWORD FAR *pcbConnStrOut,
				 UWORD fDriverCompletion)
{
	static char *func = "SQLDriverConnect";
	ConnectionClass *conn = (ConnectionClass *) hdbc;
	ConnInfo   *ci;

#ifdef WIN32
	RETCODE		dialog_result;

#endif
	RETCODE		result;
	char		connStrIn[MAX_CONNECT_STRING];
	char		connStrOut[MAX_CONNECT_STRING];
	int			retval;
	char		password_required = FALSE;
	int			len = 0;


	mylog("%s: entering...\n", func);

	if (!conn)
	{
		CC_log_error(func, "", NULL);
		return SQL_INVALID_HANDLE;
	}

	make_string(szConnStrIn, cbConnStrIn, connStrIn);

	mylog("**** SQLDriverConnect: fDriverCompletion=%d, connStrIn='%s'\n", fDriverCompletion, connStrIn);
	qlog("conn=%u, SQLDriverConnect( in)='%s', fDriverCompletion=%d\n", conn, connStrIn, fDriverCompletion);

	ci = &(conn->connInfo);

	/* Parse the connect string and fill in conninfo for this hdbc. */
	dconn_get_connect_attributes(connStrIn, ci);

	/* If the ConnInfo in the hdbc is missing anything, */
	/* this function will fill them in from the registry (assuming */
	/* of course there is a DSN given -- if not, it does nothing!) */
	getDSNinfo(ci, CONN_DONT_OVERWRITE);

	/* Fill in any default parameters if they are not there. */
	getDSNdefaults(ci);
	/* initialize pg_version */
	CC_initialize_pg_version(conn);

#ifdef WIN32
dialog:
#endif
	ci->focus_password = password_required;

	switch (fDriverCompletion)
	{
#ifdef WIN32
		case SQL_DRIVER_PROMPT:
			dialog_result = dconn_DoDialog(hwnd, ci);
			if (dialog_result != SQL_SUCCESS)
				return dialog_result;
			break;

		case SQL_DRIVER_COMPLETE_REQUIRED:

			/* Fall through */

		case SQL_DRIVER_COMPLETE:

			/* Password is not a required parameter. */
			if (ci->username[0] == '\0' ||
				ci->server[0] == '\0' ||
				ci->database[0] == '\0' ||
				ci->port[0] == '\0' ||
				password_required)
			{
				dialog_result = dconn_DoDialog(hwnd, ci);
				if (dialog_result != SQL_SUCCESS)
					return dialog_result;
			}
			break;
#else
		case SQL_DRIVER_PROMPT:
		case SQL_DRIVER_COMPLETE:
		case SQL_DRIVER_COMPLETE_REQUIRED:
#endif
		case SQL_DRIVER_NOPROMPT:
			break;
	}

	/*
	 * Password is not a required parameter unless authentication asks for
	 * it. For now, I think it's better to just let the application ask
	 * over and over until a password is entered (the user can always hit
	 * Cancel to get out)
	 */
	if (ci->username[0] == '\0' ||
		ci->server[0] == '\0' ||
		ci->database[0] == '\0' ||
		ci->port[0] == '\0')
	{
/*		(password_required && ci->password[0] == '\0')) */

		return SQL_NO_DATA_FOUND;
	}


	/* do the actual connect */
	retval = CC_connect(conn, password_required);
	if (retval < 0)
	{							/* need a password */
		if (fDriverCompletion == SQL_DRIVER_NOPROMPT)
		{
			CC_log_error(func, "Need password but Driver_NoPrompt", conn);
			return SQL_ERROR;	/* need a password but not allowed to
								 * prompt so error */
		}
		else
		{
#ifdef WIN32
			password_required = TRUE;
			goto dialog;
#else
			return SQL_ERROR;	/* until a better solution is found. */
#endif
		}
	}
	else if (retval == 0)
	{
		/* error msg filled in above */
		CC_log_error(func, "Error from CC_Connect", conn);
		return SQL_ERROR;
	}

	/*********************************************/
	/* Create the Output Connection String	 */
	/*********************************************/
	result = SQL_SUCCESS;

	makeConnectString(connStrOut, ci);
	len = strlen(connStrOut);

	if (szConnStrOut)
	{

		/*
		 * Return the completed string to the caller. The correct method
		 * is to only construct the connect string if a dialog was put up,
		 * otherwise, it should just copy the connection input string to
		 * the output. However, it seems ok to just always construct an
		 * output string.  There are possible bad side effects on working
		 * applications (Access) by implementing the correct behavior,
		 * anyway.
		 */
		strncpy_null(szConnStrOut, connStrOut, cbConnStrOutMax);

		if (len >= cbConnStrOutMax)
		{
			result = SQL_SUCCESS_WITH_INFO;
			conn->errornumber = CONN_TRUNCATED;
			conn->errormsg = "The buffer was too small for the result.";
		}
	}

	if (pcbConnStrOut)
		*pcbConnStrOut = len;

	mylog("szConnStrOut = '%s'\n", szConnStrOut);
	qlog("conn=%u, SQLDriverConnect(out)='%s'\n", conn, szConnStrOut);


	mylog("SQLDRiverConnect: returning %d\n", result);
	return result;
}

#ifdef WIN32
RETCODE
dconn_DoDialog(HWND hwnd, ConnInfo *ci)
{
	int			dialog_result;

	mylog("dconn_DoDialog: ci = %u\n", ci);

	if (hwnd)
	{
		dialog_result = DialogBoxParam(s_hModule, MAKEINTRESOURCE(DLG_CONFIG),
							hwnd, dconn_FDriverConnectProc, (LPARAM) ci);
		if (!dialog_result || (dialog_result == -1))
			return SQL_NO_DATA_FOUND;
		else
			return SQL_SUCCESS;
	}

	return SQL_ERROR;
}


BOOL FAR	PASCAL
dconn_FDriverConnectProc(
						 HWND hdlg,
						 UINT wMsg,
						 WPARAM wParam,
						 LPARAM lParam)
{
	ConnInfo   *ci;

	switch (wMsg)
	{
		case WM_INITDIALOG:
			ci = (ConnInfo *) lParam;

			/* Change the caption for the setup dialog */
			SetWindowText(hdlg, "PostgreSQL Connection");

			SetWindowText(GetDlgItem(hdlg, IDC_DATASOURCE), "Connection");

			/* Hide the DSN and description fields */
			ShowWindow(GetDlgItem(hdlg, IDC_DSNAMETEXT), SW_HIDE);
			ShowWindow(GetDlgItem(hdlg, IDC_DSNAME), SW_HIDE);
			ShowWindow(GetDlgItem(hdlg, IDC_DESCTEXT), SW_HIDE);
			ShowWindow(GetDlgItem(hdlg, IDC_DESC), SW_HIDE);

			SetWindowLong(hdlg, DWL_USER, lParam);		/* Save the ConnInfo for
														 * the "OK" */

			SetDlgStuff(hdlg, ci);

			if (ci->database[0] == '\0')
				;				/* default focus */
			else if (ci->server[0] == '\0')
				SetFocus(GetDlgItem(hdlg, IDC_SERVER));
			else if (ci->port[0] == '\0')
				SetFocus(GetDlgItem(hdlg, IDC_PORT));
			else if (ci->username[0] == '\0')
				SetFocus(GetDlgItem(hdlg, IDC_USER));
			else if (ci->focus_password)
				SetFocus(GetDlgItem(hdlg, IDC_PASSWORD));


			break;

		case WM_COMMAND:
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case IDOK:

					ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);

					GetDlgStuff(hdlg, ci);


				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;

				case IDC_DRIVER:

					DialogBoxParam(s_hModule, MAKEINTRESOURCE(DLG_OPTIONS_DRV),
								hdlg, driver_optionsProc, (LPARAM) NULL);


					break;

				case IDC_DATASOURCE:

					ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);
					DialogBoxParam(s_hModule, MAKEINTRESOURCE(DLG_OPTIONS_DS),
								   hdlg, ds_optionsProc, (LPARAM) ci);

					break;
			}
	}

	return FALSE;
}

#endif	 /* WIN32 */

void
dconn_get_connect_attributes(UCHAR FAR *connect_string, ConnInfo *ci)
{
	char	   *our_connect_string;
	char	   *pair,
			   *attribute,
			   *value,
			   *equals;
	char	   *strtok_arg;

	memset(ci, 0, sizeof(ConnInfo));

	our_connect_string = strdup(connect_string);
	strtok_arg = our_connect_string;

	mylog("our_connect_string = '%s'\n", our_connect_string);

	while (1)
	{
		pair = strtok(strtok_arg, ";");
		if (strtok_arg)
			strtok_arg = 0;
		if (!pair)
			break;

		equals = strchr(pair, '=');
		if (!equals)
			continue;

		*equals = '\0';
		attribute = pair;		/* ex. DSN */
		value = equals + 1;		/* ex. 'CEO co1' */

		mylog("attribute = '%s', value = '%s'\n", attribute, value);

		if (!attribute || !value)
			continue;

		/* Copy the appropriate value to the conninfo  */
		copyAttributes(ci, attribute, value);
	}


	free(our_connect_string);
}
