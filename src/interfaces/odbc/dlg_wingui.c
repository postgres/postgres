#ifdef	WIN32
/*-------
 * Module:			dlg_wingui.c
 *
 * Description:		This module contains any specific code for handling
 *					dialog boxes such as driver/datasource options.  Both the
 *					ConfigDSN() and the SQLDriverConnect() functions use
 *					functions in this module.  If you were to add a new option
 *					to any dialog box, you would most likely only have to change
 *					things in here rather than in 2 separate places as before.
 *
 * Classes:			none
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *-------
 */
/* Multibyte support	Eiji Tokuya 2001-03-15 */

#include "dlg_specific.h"

#include "convert.h"

#ifdef MULTIBYTE
#include "multibyte.h"
#endif
#include "pgapifunc.h"

#ifndef BOOL
#define BOOL	int
#endif
#ifndef FALSE
#define FALSE	(BOOL)0
#endif
#ifndef TRUE
#define TRUE	(BOOL)1
#endif

extern GLOBAL_VALUES globals;

extern HINSTANCE NEAR s_hModule;
static int	driver_optionsDraw(HWND, const ConnInfo *, int src, BOOL enable);
static int	driver_options_update(HWND hdlg, ConnInfo *ci, BOOL);

void
SetDlgStuff(HWND hdlg, const ConnInfo *ci)
{
	/*
	 * If driver attribute NOT present, then set the datasource name and
	 * description
	 */
	if (ci->driver[0] == '\0')
	{
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


static int
driver_optionsDraw(HWND hdlg, const ConnInfo *ci, int src, BOOL enable)
{
	const GLOBAL_VALUES *comval;
	static BOOL defset = FALSE;
	static GLOBAL_VALUES defval;

	switch (src)
	{
		case 0:			/* driver common */
			comval = &globals;
			break;
		case 1:			/* dsn specific */
			comval = &(ci->drivers);
			break;
		case 2:			/* default */
			if (!defset)
			{
				defval.commlog = DEFAULT_COMMLOG;
				defval.disable_optimizer = DEFAULT_OPTIMIZER;
				defval.ksqo = DEFAULT_KSQO;
				defval.unique_index = DEFAULT_UNIQUEINDEX;
				defval.onlyread = DEFAULT_READONLY;
				defval.use_declarefetch = DEFAULT_USEDECLAREFETCH;

				defval.parse = DEFAULT_PARSE;
				defval.cancel_as_freestmt = DEFAULT_CANCELASFREESTMT;
				defval.debug = DEFAULT_DEBUG;

				/* Unknown Sizes */
				defval.unknown_sizes = DEFAULT_UNKNOWNSIZES;
				defval.text_as_longvarchar = DEFAULT_TEXTASLONGVARCHAR;
				defval.unknowns_as_longvarchar = DEFAULT_UNKNOWNSASLONGVARCHAR;
				defval.bools_as_char = DEFAULT_BOOLSASCHAR;
			}
			defset = TRUE;
			comval = &defval;
			break;
	}

	ShowWindow(GetDlgItem(hdlg, DRV_MSG_LABEL2), enable ? SW_SHOW : SW_HIDE);
	CheckDlgButton(hdlg, DRV_COMMLOG, comval->commlog);
#ifndef Q_LOG
	EnableWindow(GetDlgItem(hdlg, DRV_COMMLOG), FALSE);
#endif /* Q_LOG */
	CheckDlgButton(hdlg, DRV_OPTIMIZER, comval->disable_optimizer);
	CheckDlgButton(hdlg, DRV_KSQO, comval->ksqo);
	CheckDlgButton(hdlg, DRV_UNIQUEINDEX, comval->unique_index);
	/* EnableWindow(GetDlgItem(hdlg, DRV_UNIQUEINDEX), enable); */
	CheckDlgButton(hdlg, DRV_READONLY, comval->onlyread);
	EnableWindow(GetDlgItem(hdlg, DRV_READONLY), enable);
	CheckDlgButton(hdlg, DRV_USEDECLAREFETCH, comval->use_declarefetch);

	/* Unknown Sizes clear */
	CheckDlgButton(hdlg, DRV_UNKNOWN_DONTKNOW, 0);
	CheckDlgButton(hdlg, DRV_UNKNOWN_LONGEST, 0);
	CheckDlgButton(hdlg, DRV_UNKNOWN_MAX, 0);
	/* Unknown (Default) Data Type sizes */
	switch (comval->unknown_sizes)
	{
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

	CheckDlgButton(hdlg, DRV_TEXT_LONGVARCHAR, comval->text_as_longvarchar);
	CheckDlgButton(hdlg, DRV_UNKNOWNS_LONGVARCHAR, comval->unknowns_as_longvarchar);
	CheckDlgButton(hdlg, DRV_BOOLS_CHAR, comval->bools_as_char);
	CheckDlgButton(hdlg, DRV_PARSE, comval->parse);
	CheckDlgButton(hdlg, DRV_CANCELASFREESTMT, comval->cancel_as_freestmt);
	CheckDlgButton(hdlg, DRV_DEBUG, comval->debug);
#ifndef MY_LOG
	EnableWindow(GetDlgItem(hdlg, DRV_DEBUG), FALSE);
#endif /* MY_LOG */
	SetDlgItemInt(hdlg, DRV_CACHE_SIZE, comval->fetch_max, FALSE);
	SetDlgItemInt(hdlg, DRV_VARCHAR_SIZE, comval->max_varchar_size, FALSE);
	SetDlgItemInt(hdlg, DRV_LONGVARCHAR_SIZE, comval->max_longvarchar_size, TRUE);
	SetDlgItemText(hdlg, DRV_EXTRASYSTABLEPREFIXES, comval->extra_systable_prefixes);

	/* Driver Connection Settings */
	SetDlgItemText(hdlg, DRV_CONNSETTINGS, comval->conn_settings);
	EnableWindow(GetDlgItem(hdlg, DRV_CONNSETTINGS), enable);
	ShowWindow(GetDlgItem(hdlg, IDPREVPAGE), enable ? SW_HIDE : SW_SHOW);
	ShowWindow(GetDlgItem(hdlg, IDNEXTPAGE), enable ? SW_HIDE : SW_SHOW);
	return 0;
}
static int
driver_options_update(HWND hdlg, ConnInfo *ci, BOOL updateProfile)
{
	GLOBAL_VALUES *comval;

	if (ci)
		comval = &(ci->drivers);
	else
		comval = &globals;
	comval->commlog = IsDlgButtonChecked(hdlg, DRV_COMMLOG);
	comval->disable_optimizer = IsDlgButtonChecked(hdlg, DRV_OPTIMIZER);
	comval->ksqo = IsDlgButtonChecked(hdlg, DRV_KSQO);
	comval->unique_index = IsDlgButtonChecked(hdlg, DRV_UNIQUEINDEX);
	if (!ci)
	{
		comval->onlyread = IsDlgButtonChecked(hdlg, DRV_READONLY);
	}
	comval->use_declarefetch = IsDlgButtonChecked(hdlg, DRV_USEDECLAREFETCH);

	/* Unknown (Default) Data Type sizes */
	if (IsDlgButtonChecked(hdlg, DRV_UNKNOWN_MAX))
		comval->unknown_sizes = UNKNOWNS_AS_MAX;
	else if (IsDlgButtonChecked(hdlg, DRV_UNKNOWN_DONTKNOW))
		comval->unknown_sizes = UNKNOWNS_AS_DONTKNOW;
	else if (IsDlgButtonChecked(hdlg, DRV_UNKNOWN_LONGEST))
		comval->unknown_sizes = UNKNOWNS_AS_LONGEST;
	else
		comval->unknown_sizes = UNKNOWNS_AS_MAX;

	comval->text_as_longvarchar = IsDlgButtonChecked(hdlg, DRV_TEXT_LONGVARCHAR);
	comval->unknowns_as_longvarchar = IsDlgButtonChecked(hdlg, DRV_UNKNOWNS_LONGVARCHAR);
	comval->bools_as_char = IsDlgButtonChecked(hdlg, DRV_BOOLS_CHAR);

	comval->parse = IsDlgButtonChecked(hdlg, DRV_PARSE);

	comval->cancel_as_freestmt = IsDlgButtonChecked(hdlg, DRV_CANCELASFREESTMT);
	comval->debug = IsDlgButtonChecked(hdlg, DRV_DEBUG);

	comval->fetch_max = GetDlgItemInt(hdlg, DRV_CACHE_SIZE, NULL, FALSE);
	comval->max_varchar_size = GetDlgItemInt(hdlg, DRV_VARCHAR_SIZE, NULL, FALSE);
	comval->max_longvarchar_size = GetDlgItemInt(hdlg, DRV_LONGVARCHAR_SIZE, NULL, TRUE);		/* allows for
																								 * SQL_NO_TOTAL */

	GetDlgItemText(hdlg, DRV_EXTRASYSTABLEPREFIXES, comval->extra_systable_prefixes, sizeof(comval->extra_systable_prefixes));

	/* Driver Connection Settings */
	if (!ci)
		GetDlgItemText(hdlg, DRV_CONNSETTINGS, comval->conn_settings, sizeof(comval->conn_settings));

	if (updateProfile)
		writeDriverCommoninfo(ci);

	/* fall through */
	return 0;
}

int			CALLBACK
driver_optionsProc(HWND hdlg,
				   UINT wMsg,
				   WPARAM wParam,
				   LPARAM lParam)
{
	ConnInfo   *ci;

	switch (wMsg)
	{
		case WM_INITDIALOG:
			SetWindowLong(hdlg, DWL_USER, lParam);		/* save for OK etc */
			ci = (ConnInfo *) lParam;
			SetWindowText(hdlg, "Advanced Options (Default)");
			SetWindowText(GetDlgItem(hdlg, IDOK), "Save");
			ShowWindow(GetDlgItem(hdlg, IDAPPLY), SW_HIDE);
			driver_optionsDraw(hdlg, ci, 0, TRUE);
			break;

		case WM_COMMAND:
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case IDOK:
					ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);
					driver_options_update(hdlg, NULL,
										  ci && ci->dsn && ci->dsn[0]);

				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;

				case IDDEFAULTS:
					driver_optionsDraw(hdlg, NULL, 2, TRUE);
					break;
			}
	}

	return FALSE;
}

int			CALLBACK
global_optionsProc(HWND hdlg,
				   UINT wMsg,
				   WPARAM wParam,
				   LPARAM lParam)
{

	switch (wMsg)
	{
		case WM_INITDIALOG:
			CheckDlgButton(hdlg, DRV_COMMLOG, globals.commlog);
#ifndef Q_LOG
			EnableWindow(GetDlgItem(hdlg, DRV_COMMLOG), FALSE);
#endif /* Q_LOG */
			CheckDlgButton(hdlg, DRV_DEBUG, globals.debug);
#ifndef MY_LOG
			EnableWindow(GetDlgItem(hdlg, DRV_DEBUG), FALSE);
#endif /* MY_LOG */
			break;

		case WM_COMMAND:
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case IDOK:
					globals.commlog = IsDlgButtonChecked(hdlg, DRV_COMMLOG);
					globals.debug = IsDlgButtonChecked(hdlg, DRV_DEBUG);
					driver_options_update(hdlg, NULL, TRUE);

				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;
			}
	}

	return FALSE;
}

int			CALLBACK
ds_options1Proc(HWND hdlg,
				   UINT wMsg,
				   WPARAM wParam,
				   LPARAM lParam)
{
	ConnInfo   *ci;

	switch (wMsg)
	{
		case WM_INITDIALOG:
			SetWindowLong(hdlg, DWL_USER, lParam);		/* save for OK etc */
			ci = (ConnInfo *) lParam;
			if (ci && ci->dsn && ci->dsn[0])
				SetWindowText(hdlg, "Advanced Options (DSN 1/2)");
			else
			{
				SetWindowText(hdlg, "Advanced Options (Connection 1/2)");
				ShowWindow(GetDlgItem(hdlg, IDAPPLY), SW_HIDE);
			}
			driver_optionsDraw(hdlg, ci, 1, FALSE);
			break;

		case WM_COMMAND:
			ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case IDOK:
					driver_options_update(hdlg, ci, FALSE);

				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;

				case IDAPPLY:
					driver_options_update(hdlg, ci, FALSE);
					SendMessage(GetWindow(hdlg, GW_OWNER), WM_COMMAND, wParam, lParam);
					break;

				case IDDEFAULTS:
					driver_optionsDraw(hdlg, ci, 0, FALSE);
					break;

				case IDNEXTPAGE:
					driver_options_update(hdlg, ci, FALSE);

					EndDialog(hdlg, FALSE);
					DialogBoxParam(s_hModule,
						MAKEINTRESOURCE(DLG_OPTIONS_DS),
                                                 	hdlg, ds_options2Proc, (LPARAM)
ci);
					break;
			}
	}

	return FALSE;
}


int			CALLBACK
ds_options2Proc(HWND hdlg,
			   UINT wMsg,
			   WPARAM wParam,
			   LPARAM lParam)
{
	ConnInfo   *ci;
	char		buf[128];
	DWORD		cmd;

	switch (wMsg)
	{
		case WM_INITDIALOG:
			ci = (ConnInfo *) lParam;
			SetWindowLong(hdlg, DWL_USER, lParam);		/* save for OK */

			/* Change window caption */
			if (ci->driver[0])
			{
				SetWindowText(hdlg, "Advanced Options (Connection 2/2)");
				ShowWindow(GetDlgItem(hdlg, IDAPPLY), SW_HIDE);				}
			else
			{
				sprintf(buf, "Advanced Options (%s) 2/2", ci->dsn);
				SetWindowText(hdlg, buf);
			}

			/* Readonly */
			CheckDlgButton(hdlg, DS_READONLY, atoi(ci->onlyread));

			/* Protocol */
			if (strncmp(ci->protocol, PG62, strlen(PG62)) == 0)
				CheckDlgButton(hdlg, DS_PG62, 1);
			else if (strncmp(ci->protocol, PG63, strlen(PG63)) == 0)
				CheckDlgButton(hdlg, DS_PG63, 1);
			else
				/* latest */
				CheckDlgButton(hdlg, DS_PG64, 1);

			/* Int8 As */
			switch (ci->int8_as)
			{
				case SQL_BIGINT:
					CheckDlgButton(hdlg, DS_INT8_AS_BIGINT, 1);
					break;
				case SQL_NUMERIC:
					CheckDlgButton(hdlg, DS_INT8_AS_NUMERIC, 1);
					break;
				case SQL_VARCHAR:
					CheckDlgButton(hdlg, DS_INT8_AS_VARCHAR, 1);
					break;
				case SQL_DOUBLE:
					CheckDlgButton(hdlg, DS_INT8_AS_DOUBLE, 1);
					break;
				case SQL_INTEGER:
					CheckDlgButton(hdlg, DS_INT8_AS_INT4, 1);
					break;
				default:
					CheckDlgButton(hdlg, DS_INT8_AS_DEFAULT, 1);
			}

			CheckDlgButton(hdlg, DS_SHOWOIDCOLUMN, atoi(ci->show_oid_column));
			CheckDlgButton(hdlg, DS_FAKEOIDINDEX, atoi(ci->fake_oid_index));
			CheckDlgButton(hdlg, DS_ROWVERSIONING, atoi(ci->row_versioning));
			CheckDlgButton(hdlg, DS_SHOWSYSTEMTABLES, atoi(ci->show_system_tables));
			CheckDlgButton(hdlg, DS_DISALLOWPREMATURE, ci->disallow_premature);
			CheckDlgButton(hdlg, DS_LFCONVERSION, ci->lf_conversion);
			CheckDlgButton(hdlg, DS_TRUEISMINUS1, ci->true_is_minus1);
			CheckDlgButton(hdlg, DS_UPDATABLECURSORS, ci->allow_keyset);
#ifndef DRIVER_CURSOR_IMPLEMENT
			EnableWindow(GetDlgItem(hdlg, DS_UPDATABLECURSORS), FALSE);
#endif /* DRIVER_CURSOR_IMPLEMENT */

			EnableWindow(GetDlgItem(hdlg, DS_FAKEOIDINDEX), atoi(ci->show_oid_column));

			/* Datasource Connection Settings */
			SetDlgItemText(hdlg, DS_CONNSETTINGS, ci->conn_settings);
			break;

		case WM_COMMAND:
			switch (cmd = GET_WM_COMMAND_ID(wParam, lParam))
			{
				case DS_SHOWOIDCOLUMN:
					mylog("WM_COMMAND: DS_SHOWOIDCOLUMN\n");
					EnableWindow(GetDlgItem(hdlg, DS_FAKEOIDINDEX), IsDlgButtonChecked(hdlg, DS_SHOWOIDCOLUMN));
					return TRUE;

				case IDOK:
				case IDAPPLY:
				case IDPREVPAGE:
					ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);
					mylog("IDOK: got ci = %u\n", ci);

					/* Readonly */
					sprintf(ci->onlyread, "%d", IsDlgButtonChecked(hdlg, DS_READONLY));

					/* Protocol */
					if (IsDlgButtonChecked(hdlg, DS_PG62))
						strcpy(ci->protocol, PG62);
					else if (IsDlgButtonChecked(hdlg, DS_PG63))
						strcpy(ci->protocol, PG63);
					else
						/* latest */
						strcpy(ci->protocol, PG64);

					/* Int8 As */
					if (IsDlgButtonChecked(hdlg, DS_INT8_AS_DEFAULT))
						ci->int8_as = 0;
					else if (IsDlgButtonChecked(hdlg, DS_INT8_AS_BIGINT))
						ci->int8_as = SQL_BIGINT;
					else if (IsDlgButtonChecked(hdlg, DS_INT8_AS_NUMERIC))
						ci->int8_as = SQL_NUMERIC;
					else if (IsDlgButtonChecked(hdlg, DS_INT8_AS_DOUBLE))
						ci->int8_as = SQL_DOUBLE;
					else if (IsDlgButtonChecked(hdlg, DS_INT8_AS_INT4))
						ci->int8_as = SQL_INTEGER;
					else
						ci->int8_as = SQL_VARCHAR;

					sprintf(ci->show_system_tables, "%d", IsDlgButtonChecked(hdlg, DS_SHOWSYSTEMTABLES));

					sprintf(ci->row_versioning, "%d", IsDlgButtonChecked(hdlg, DS_ROWVERSIONING));
					ci->disallow_premature = IsDlgButtonChecked(hdlg, DS_DISALLOWPREMATURE);
					ci->lf_conversion = IsDlgButtonChecked(hdlg, DS_LFCONVERSION);
					ci->true_is_minus1 = IsDlgButtonChecked(hdlg, DS_TRUEISMINUS1);
#ifdef DRIVER_CURSOR_IMPLEMENT
					ci->allow_keyset = IsDlgButtonChecked(hdlg, DS_UPDATABLECURSORS);
#endif /* DRIVER_CURSOR_IMPLEMENT */

					/* OID Options */
					sprintf(ci->fake_oid_index, "%d", IsDlgButtonChecked(hdlg, DS_FAKEOIDINDEX));
					sprintf(ci->show_oid_column, "%d", IsDlgButtonChecked(hdlg, DS_SHOWOIDCOLUMN));

					/* Datasource Connection Settings */
					GetDlgItemText(hdlg, DS_CONNSETTINGS, ci->conn_settings, sizeof(ci->conn_settings));
					if (IDAPPLY == cmd)
					{
						SendMessage(GetWindow(hdlg, GW_OWNER), WM_COMMAND, wParam, lParam);
						break;
					}

					EndDialog(hdlg, cmd == IDOK);
					if (IDOK == cmd) 
						return TRUE;
					DialogBoxParam(s_hModule,
						MAKEINTRESOURCE(DLG_OPTIONS_DRV),
                                         	hdlg, ds_options1Proc, (LPARAM) ci);
					break;

				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;
			}
	}

	return FALSE;
}

#endif /* WIN32 */
