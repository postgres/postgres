/*-------
 * Module:			dlg_specific.c
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

#ifdef	WIN32
static int	driver_optionsDraw(HWND, const ConnInfo *, int src, BOOL enable);
static int	driver_options_update(HWND hdlg, ConnInfo *ci, BOOL);
static void updateCommons(const ConnInfo *ci);
#endif

#ifdef WIN32
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

	CheckDlgButton(hdlg, DRV_COMMLOG, comval->commlog);
	CheckDlgButton(hdlg, DRV_OPTIMIZER, comval->disable_optimizer);
	CheckDlgButton(hdlg, DRV_KSQO, comval->ksqo);
	CheckDlgButton(hdlg, DRV_UNIQUEINDEX, comval->unique_index);
	EnableWindow(GetDlgItem(hdlg, DRV_UNIQUEINDEX), enable);
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
	SetDlgItemInt(hdlg, DRV_CACHE_SIZE, comval->fetch_max, FALSE);
	SetDlgItemInt(hdlg, DRV_VARCHAR_SIZE, comval->max_varchar_size, FALSE);
	SetDlgItemInt(hdlg, DRV_LONGVARCHAR_SIZE, comval->max_longvarchar_size, TRUE);
	SetDlgItemText(hdlg, DRV_EXTRASYSTABLEPREFIXES, comval->extra_systable_prefixes);

	/* Driver Connection Settings */
	SetDlgItemText(hdlg, DRV_CONNSETTINGS, comval->conn_settings);
	EnableWindow(GetDlgItem(hdlg, DRV_CONNSETTINGS), enable);
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
	if (!ci)
	{
		comval->unique_index = IsDlgButtonChecked(hdlg, DRV_UNIQUEINDEX);
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
		updateCommons(ci);

	/* fall through */
	return 0;
}

int			CALLBACK
driver_optionsProc(HWND hdlg,
				   WORD wMsg,
				   WPARAM wParam,
				   LPARAM lParam)
{
	ConnInfo   *ci;

	switch (wMsg)
	{
		case WM_INITDIALOG:
			SetWindowLong(hdlg, DWL_USER, lParam);		/* save for OK etc */
			ci = (ConnInfo *) lParam;
			CheckDlgButton(hdlg, DRV_OR_DSN, 0);
			if (ci && ci->dsn && ci->dsn[0])
				SetWindowText(hdlg, "Advanced Options (per DSN)");
			else
			{
				SetWindowText(hdlg, "Advanced Options (Connection)");
				ShowWindow(GetDlgItem(hdlg, DRV_OR_DSN), SW_HIDE);
			}
			driver_optionsDraw(hdlg, ci, 1, FALSE);
			break;

		case WM_COMMAND:
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case IDOK:
					ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);
					driver_options_update(hdlg, IsDlgButtonChecked(hdlg, DRV_OR_DSN) ? NULL : ci,
										  ci && ci->dsn && ci->dsn[0]);

				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;

				case IDDEFAULTS:
					if (IsDlgButtonChecked(hdlg, DRV_OR_DSN))
						driver_optionsDraw(hdlg, NULL, 2, TRUE);
					else
					{
						ConnInfo   *ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);

						driver_optionsDraw(hdlg, ci, 0, FALSE);
					}
					break;

				case DRV_OR_DSN:
					if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED)
					{
						mylog("DRV_OR_DSN clicked\n");
						if (IsDlgButtonChecked(hdlg, DRV_OR_DSN))
						{
							SetWindowText(hdlg, "Advanced Options (Common)");
							driver_optionsDraw(hdlg, NULL, 0, TRUE);
						}
						else
						{
							ConnInfo   *ci = (ConnInfo *) GetWindowLong(hdlg, DWL_USER);

							SetWindowText(hdlg, "Advanced Options (per DSN)");
							driver_optionsDraw(hdlg, ci, ci ? 1 : 0, ci == NULL);
						}
					}
					break;
			}
	}

	return FALSE;
}


int			CALLBACK
ds_optionsProc(HWND hdlg,
			   WORD wMsg,
			   WPARAM wParam,
			   LPARAM lParam)
{
	ConnInfo   *ci;
	char		buf[128];

	switch (wMsg)
	{
		case WM_INITDIALOG:
			ci = (ConnInfo *) lParam;
			SetWindowLong(hdlg, DWL_USER, lParam);		/* save for OK */

			/* Change window caption */
			if (ci->driver[0])
				SetWindowText(hdlg, "Advanced Options (Connection)");
			else
			{
				sprintf(buf, "Advanced Options (%s)", ci->dsn);
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

			CheckDlgButton(hdlg, DS_SHOWOIDCOLUMN, atoi(ci->show_oid_column));
			CheckDlgButton(hdlg, DS_FAKEOIDINDEX, atoi(ci->fake_oid_index));
			CheckDlgButton(hdlg, DS_ROWVERSIONING, atoi(ci->row_versioning));
			CheckDlgButton(hdlg, DS_SHOWSYSTEMTABLES, atoi(ci->show_system_tables));
			CheckDlgButton(hdlg, DS_DISALLOWPREMATURE, ci->disallow_premature);

			EnableWindow(GetDlgItem(hdlg, DS_FAKEOIDINDEX), atoi(ci->show_oid_column));

			/* Datasource Connection Settings */
			SetDlgItemText(hdlg, DS_CONNSETTINGS, ci->conn_settings);
			break;

		case WM_COMMAND:
			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
				case DS_SHOWOIDCOLUMN:
					mylog("WM_COMMAND: DS_SHOWOIDCOLUMN\n");
					EnableWindow(GetDlgItem(hdlg, DS_FAKEOIDINDEX), IsDlgButtonChecked(hdlg, DS_SHOWOIDCOLUMN));
					return TRUE;

				case IDOK:
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

					sprintf(ci->show_system_tables, "%d", IsDlgButtonChecked(hdlg, DS_SHOWSYSTEMTABLES));

					sprintf(ci->row_versioning, "%d", IsDlgButtonChecked(hdlg, DS_ROWVERSIONING));
					ci->disallow_premature = IsDlgButtonChecked(hdlg, DS_DISALLOWPREMATURE);

					/* OID Options */
					sprintf(ci->fake_oid_index, "%d", IsDlgButtonChecked(hdlg, DS_FAKEOIDINDEX));
					sprintf(ci->show_oid_column, "%d", IsDlgButtonChecked(hdlg, DS_SHOWOIDCOLUMN));

					/* Datasource Connection Settings */
					GetDlgItemText(hdlg, DS_CONNSETTINGS, ci->conn_settings, sizeof(ci->conn_settings));

					/* fall through */

				case IDCANCEL:
					EndDialog(hdlg, GET_WM_COMMAND_ID(wParam, lParam) == IDOK);
					return TRUE;
			}
	}

	return FALSE;
}

/*
 *	This function writes any global parameters (that can be manipulated)
 *	to the ODBCINST.INI portion of the registry
 */
static void
updateCommons(const ConnInfo *ci)
{
	const char *sectionName;
	const char *fileName;
	const GLOBAL_VALUES *comval;
	char		tmp[128];

	if (ci)
		if (ci->dsn && ci->dsn[0])
		{
			mylog("DSN=%s updating\n", ci->dsn);
			comval = &(ci->drivers);
			sectionName = ci->dsn;
			fileName = ODBC_INI;
		}
		else
		{
			mylog("ci but dsn==NULL\n");
			return;
		}
	else
	{
		mylog("drivers updating\n");
		comval = &globals;
		sectionName = DBMS_NAME;
		fileName = ODBCINST_INI;
	}
	sprintf(tmp, "%d", comval->fetch_max);
	SQLWritePrivateProfileString(sectionName,
								 INI_FETCH, tmp, fileName);

	sprintf(tmp, "%d", comval->commlog);
	SQLWritePrivateProfileString(sectionName,
								 INI_COMMLOG, tmp, fileName);

	sprintf(tmp, "%d", comval->debug);
	SQLWritePrivateProfileString(sectionName,
								 INI_DEBUG, tmp, fileName);

	sprintf(tmp, "%d", comval->disable_optimizer);
	SQLWritePrivateProfileString(sectionName,
								 INI_OPTIMIZER, tmp, fileName);

	sprintf(tmp, "%d", comval->ksqo);
	SQLWritePrivateProfileString(sectionName,
								 INI_KSQO, tmp, fileName);

	/*
	 * Never update the onlyread, unique_index from this module.
	 */
	if (!ci)
	{
		sprintf(tmp, "%d", comval->unique_index);
		SQLWritePrivateProfileString(sectionName, INI_UNIQUEINDEX, tmp,
									 fileName);

		sprintf(tmp, "%d", comval->onlyread);
		SQLWritePrivateProfileString(sectionName, INI_READONLY, tmp,
									 fileName);
	}

	sprintf(tmp, "%d", comval->use_declarefetch);
	SQLWritePrivateProfileString(sectionName,
								 INI_USEDECLAREFETCH, tmp, fileName);

	sprintf(tmp, "%d", comval->unknown_sizes);
	SQLWritePrivateProfileString(sectionName,
								 INI_UNKNOWNSIZES, tmp, fileName);

	sprintf(tmp, "%d", comval->text_as_longvarchar);
	SQLWritePrivateProfileString(sectionName,
								 INI_TEXTASLONGVARCHAR, tmp, fileName);

	sprintf(tmp, "%d", comval->unknowns_as_longvarchar);
	SQLWritePrivateProfileString(sectionName,
							   INI_UNKNOWNSASLONGVARCHAR, tmp, fileName);

	sprintf(tmp, "%d", comval->bools_as_char);
	SQLWritePrivateProfileString(sectionName,
								 INI_BOOLSASCHAR, tmp, fileName);

	sprintf(tmp, "%d", comval->parse);
	SQLWritePrivateProfileString(sectionName,
								 INI_PARSE, tmp, fileName);

	sprintf(tmp, "%d", comval->cancel_as_freestmt);
	SQLWritePrivateProfileString(sectionName,
								 INI_CANCELASFREESTMT, tmp, fileName);

	sprintf(tmp, "%d", comval->max_varchar_size);
	SQLWritePrivateProfileString(sectionName,
								 INI_MAXVARCHARSIZE, tmp, fileName);

	sprintf(tmp, "%d", comval->max_longvarchar_size);
	SQLWritePrivateProfileString(sectionName,
								 INI_MAXLONGVARCHARSIZE, tmp, fileName);

	SQLWritePrivateProfileString(sectionName,
	INI_EXTRASYSTABLEPREFIXES, comval->extra_systable_prefixes, fileName);

	/*
	 * Never update the conn_setting from this module
	 * SQLWritePrivateProfileString(sectionName, INI_CONNSETTINGS,
	 * comval->conn_settings, fileName);
	 */
}
#endif   /* WIN32 */


void
makeConnectString(char *connect_string, const ConnInfo *ci, UWORD len)
{
	char		got_dsn = (ci->dsn[0] != '\0');
	char		encoded_conn_settings[LARGE_REGISTRY_LEN];
	UWORD		hlen;
	/*BOOL		abbrev = (len <= 400);*/
	BOOL		abbrev = (len < 1024);

	/* fundamental info */
	sprintf(connect_string, "%s=%s;DATABASE=%s;SERVER=%s;PORT=%s;UID=%s;PWD=%s",
			got_dsn ? "DSN" : "DRIVER",
			got_dsn ? ci->dsn : ci->driver,
			ci->database,
			ci->server,
			ci->port,
			ci->username,
			ci->password);

	encode(ci->conn_settings, encoded_conn_settings);

	/* extra info */
	hlen = strlen(connect_string);
	if (!abbrev)
		sprintf(&connect_string[hlen],
				";READONLY=%s;PROTOCOL=%s;FAKEOIDINDEX=%s;SHOWOIDCOLUMN=%s;ROWVERSIONING=%s;SHOWSYSTEMTABLES=%s;CONNSETTINGS=%s;FETCH=%d;SOCKET=%d;UNKNOWNSIZES=%d;MAXVARCHARSIZE=%d;MAXLONGVARCHARSIZE=%d;DEBUG=%d;COMMLOG=%d;OPTIMIZER=%d;KSQO=%d;USEDECLAREFETCH=%d;TEXTASLONGVARCHAR=%d;UNKNOWNSASLONGVARCHAR=%d;BOOLSASCHAR=%d;PARSE=%d;CANCELASFREESTMT=%d;EXTRASYSTABLEPREFIXES=%s",
				ci->onlyread,
				ci->protocol,
				ci->fake_oid_index,
				ci->show_oid_column,
				ci->row_versioning,
				ci->show_system_tables,
				encoded_conn_settings,
				ci->drivers.fetch_max,
				ci->drivers.socket_buffersize,
				ci->drivers.unknown_sizes,
				ci->drivers.max_varchar_size,
				ci->drivers.max_longvarchar_size,
				ci->drivers.debug,
				ci->drivers.commlog,
				ci->drivers.disable_optimizer,
				ci->drivers.ksqo,
				ci->drivers.use_declarefetch,
				ci->drivers.text_as_longvarchar,
				ci->drivers.unknowns_as_longvarchar,
				ci->drivers.bools_as_char,
				ci->drivers.parse,
				ci->drivers.cancel_as_freestmt,
				ci->drivers.extra_systable_prefixes);
	/* Abbrebiation is needed ? */
	if (abbrev || strlen(connect_string) >= len)
		sprintf(&connect_string[hlen],
				";A0=%s;A1=%s;A2=%s;A3=%s;A4=%s;A5=%s;A6=%s;A7=%d;A8=%d;A9=%d;B0=%d;B1=%d;B2=%d;B3=%d;B4=%d;B5=%d;B6=%d;B7=%d;B8=%d;B9=%d;C0=%d;C1=%d;C2=%s",
				ci->onlyread,
				ci->protocol,
				ci->fake_oid_index,
				ci->show_oid_column,
				ci->row_versioning,
				ci->show_system_tables,
				encoded_conn_settings,
				ci->drivers.fetch_max,
				ci->drivers.socket_buffersize,
				ci->drivers.unknown_sizes,
				ci->drivers.max_varchar_size,
				ci->drivers.max_longvarchar_size,
				ci->drivers.debug,
				ci->drivers.commlog,
				ci->drivers.disable_optimizer,
				ci->drivers.ksqo,
				ci->drivers.use_declarefetch,
				ci->drivers.text_as_longvarchar,
				ci->drivers.unknowns_as_longvarchar,
				ci->drivers.bools_as_char,
				ci->drivers.parse,
				ci->drivers.cancel_as_freestmt,
				ci->drivers.extra_systable_prefixes);
}


void
copyAttributes(ConnInfo *ci, const char *attribute, const char *value)
{
	if (stricmp(attribute, "DSN") == 0)
		strcpy(ci->dsn, value);

	else if (stricmp(attribute, "driver") == 0)
		strcpy(ci->driver, value);

	else if (stricmp(attribute, INI_DATABASE) == 0)
		strcpy(ci->database, value);

	else if (stricmp(attribute, INI_SERVER) == 0 || stricmp(attribute, "server") == 0)
		strcpy(ci->server, value);

	else if (stricmp(attribute, INI_USER) == 0 || stricmp(attribute, "uid") == 0)
		strcpy(ci->username, value);

	else if (stricmp(attribute, INI_PASSWORD) == 0 || stricmp(attribute, "pwd") == 0)
		strcpy(ci->password, value);

	else if (stricmp(attribute, INI_PORT) == 0)
		strcpy(ci->port, value);

	else if (stricmp(attribute, INI_READONLY) == 0 || stricmp(attribute, "A0") == 0)
		strcpy(ci->onlyread, value);

	else if (stricmp(attribute, INI_PROTOCOL) == 0 || stricmp(attribute, "A1") == 0)
		strcpy(ci->protocol, value);

	else if (stricmp(attribute, INI_SHOWOIDCOLUMN) == 0 || stricmp(attribute, "A3") == 0)
		strcpy(ci->show_oid_column, value);

	else if (stricmp(attribute, INI_FAKEOIDINDEX) == 0 || stricmp(attribute, "A2") == 0)
		strcpy(ci->fake_oid_index, value);

	else if (stricmp(attribute, INI_ROWVERSIONING) == 0 || stricmp(attribute, "A4") == 0)
		strcpy(ci->row_versioning, value);

	else if (stricmp(attribute, INI_SHOWSYSTEMTABLES) == 0 || stricmp(attribute, "A5") == 0)
		strcpy(ci->show_system_tables, value);

	else if (stricmp(attribute, INI_CONNSETTINGS) == 0 || stricmp(attribute, "A6") == 0)
	{
		decode(value, ci->conn_settings);
		/* strcpy(ci->conn_settings, value); */
	}
	else if (stricmp(attribute, INI_DISALLOWPREMATURE) == 0 || stricmp(attribute, "C3") == 0)
		ci->disallow_premature = atoi(value);
	else if (stricmp(attribute, INI_UPDATABLECURSORS) == 0 || stricmp(attribute, "C4") == 0)
		ci->updatable_cursors = atoi(value);

	mylog("copyAttributes: DSN='%s',server='%s',dbase='%s',user='%s',passwd='%s',port='%s',onlyread='%s',protocol='%s',conn_settings='%s',disallow_premature=%d)\n", ci->dsn, ci->server, ci->database, ci->username, ci->password, ci->port, ci->onlyread, ci->protocol, ci->conn_settings, ci->disallow_premature);
}

void
copyCommonAttributes(ConnInfo *ci, const char *attribute, const char *value)
{
	if (stricmp(attribute, INI_FETCH) == 0 || stricmp(attribute, "A7") == 0)
		ci->drivers.fetch_max = atoi(value);
	else if (stricmp(attribute, INI_SOCKET) == 0 || stricmp(attribute, "A8") == 0)
		ci->drivers.socket_buffersize = atoi(value);
	else if (stricmp(attribute, INI_DEBUG) == 0 || stricmp(attribute, "B2") == 0)
		ci->drivers.debug = atoi(value);
	else if (stricmp(attribute, INI_COMMLOG) == 0 || stricmp(attribute, "B3") == 0)
		ci->drivers.commlog = atoi(value);
	else if (stricmp(attribute, INI_OPTIMIZER) == 0 || stricmp(attribute, "B4") == 0)
		ci->drivers.disable_optimizer = atoi(value);
	else if (stricmp(attribute, INI_KSQO) == 0 || stricmp(attribute, "B5") == 0)
		ci->drivers.ksqo = atoi(value);

	/*
	 * else if (stricmp(attribute, INI_UNIQUEINDEX) == 0 ||
	 * stricmp(attribute, "UIX") == 0) ci->drivers.unique_index =
	 * atoi(value);
	 */
	else if (stricmp(attribute, INI_UNKNOWNSIZES) == 0 || stricmp(attribute, "A9") == 0)
		ci->drivers.unknown_sizes = atoi(value);
	else if (stricmp(attribute, INI_LIE) == 0)
		ci->drivers.lie = atoi(value);
	else if (stricmp(attribute, INI_PARSE) == 0 || stricmp(attribute, "C0") == 0)
		ci->drivers.parse = atoi(value);
	else if (stricmp(attribute, INI_CANCELASFREESTMT) == 0 || stricmp(attribute, "C1") == 0)
		ci->drivers.cancel_as_freestmt = atoi(value);
	else if (stricmp(attribute, INI_USEDECLAREFETCH) == 0 || stricmp(attribute, "B6") == 0)
		ci->drivers.use_declarefetch = atoi(value);
	else if (stricmp(attribute, INI_MAXVARCHARSIZE) == 0 || stricmp(attribute, "B0") == 0)
		ci->drivers.max_varchar_size = atoi(value);
	else if (stricmp(attribute, INI_MAXLONGVARCHARSIZE) == 0 || stricmp(attribute, "B1") == 0)
		ci->drivers.max_longvarchar_size = atoi(value);
	else if (stricmp(attribute, INI_TEXTASLONGVARCHAR) == 0 || stricmp(attribute, "B7") == 0)
		ci->drivers.text_as_longvarchar = atoi(value);
	else if (stricmp(attribute, INI_UNKNOWNSASLONGVARCHAR) == 0 || stricmp(attribute, "B8") == 0)
		ci->drivers.unknowns_as_longvarchar = atoi(value);
	else if (stricmp(attribute, INI_BOOLSASCHAR) == 0 || stricmp(attribute, "B9") == 0)
		ci->drivers.bools_as_char = atoi(value);
	else if (stricmp(attribute, INI_EXTRASYSTABLEPREFIXES) == 0 || stricmp(attribute, "C2") == 0)
		strcpy(ci->drivers.extra_systable_prefixes, value);
	mylog("CopyCommonAttributes: A7=%d;A8=%d;A9=%d;B0=%d;B1=%d;B2=%d;B3=%d;B4=%d;B5=%d;B6=%d;B7=%d;B8=%d;B9=%d;C0=%d;C1=%d;C2=%s",
		  ci->drivers.fetch_max,
		  ci->drivers.socket_buffersize,
		  ci->drivers.unknown_sizes,
		  ci->drivers.max_varchar_size,
		  ci->drivers.max_longvarchar_size,
		  ci->drivers.debug,
		  ci->drivers.commlog,
		  ci->drivers.disable_optimizer,
		  ci->drivers.ksqo,
		  ci->drivers.use_declarefetch,
		  ci->drivers.text_as_longvarchar,
		  ci->drivers.unknowns_as_longvarchar,
		  ci->drivers.bools_as_char,
		  ci->drivers.parse,
		  ci->drivers.cancel_as_freestmt,
		  ci->drivers.extra_systable_prefixes);
}


void
getDSNdefaults(ConnInfo *ci)
{
	if (ci->port[0] == '\0')
		strcpy(ci->port, DEFAULT_PORT);

	if (ci->onlyread[0] == '\0')
		sprintf(ci->onlyread, "%d", globals.onlyread);

	if (ci->protocol[0] == '\0')
		strcpy(ci->protocol, globals.protocol);

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
	char	   *DSN = ci->dsn;
	char		encoded_conn_settings[LARGE_REGISTRY_LEN],
				temp[SMALL_REGISTRY_LEN];

/*
 *	If a driver keyword was present, then dont use a DSN and return.
 *	If DSN is null and no driver, then use the default datasource.
 */
	memcpy(&ci->drivers, &globals, sizeof(globals));
	if (DSN[0] == '\0')
	{
		if (ci->driver[0] != '\0')
			return;
		else
			strcpy(DSN, INI_DSN);
	}

	/* brute-force chop off trailing blanks... */
	while (*(DSN + strlen(DSN) - 1) == ' ')
		*(DSN + strlen(DSN) - 1) = '\0';

	/* Proceed with getting info for the given DSN. */

	if (ci->desc[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_KDESC, "", ci->desc, sizeof(ci->desc), ODBC_INI);

	if (ci->server[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_SERVER, "", ci->server, sizeof(ci->server), ODBC_INI);

	if (ci->database[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_DATABASE, "", ci->database, sizeof(ci->database), ODBC_INI);

	if (ci->username[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_USER, "", ci->username, sizeof(ci->username), ODBC_INI);

	if (ci->password[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_PASSWORD, "", ci->password, sizeof(ci->password), ODBC_INI);

	if (ci->port[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_PORT, "", ci->port, sizeof(ci->port), ODBC_INI);

	if (ci->onlyread[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_READONLY, "", ci->onlyread, sizeof(ci->onlyread), ODBC_INI);

	if (ci->show_oid_column[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_SHOWOIDCOLUMN, "", ci->show_oid_column, sizeof(ci->show_oid_column), ODBC_INI);

	if (ci->fake_oid_index[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_FAKEOIDINDEX, "", ci->fake_oid_index, sizeof(ci->fake_oid_index), ODBC_INI);

	if (ci->row_versioning[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_ROWVERSIONING, "", ci->row_versioning, sizeof(ci->row_versioning), ODBC_INI);

	if (ci->show_system_tables[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_SHOWSYSTEMTABLES, "", ci->show_system_tables, sizeof(ci->show_system_tables), ODBC_INI);

	if (ci->protocol[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_PROTOCOL, "", ci->protocol, sizeof(ci->protocol), ODBC_INI);

	if (ci->conn_settings[0] == '\0' || overwrite)
	{
		SQLGetPrivateProfileString(DSN, INI_CONNSETTINGS, "", encoded_conn_settings, sizeof(encoded_conn_settings), ODBC_INI);
		decode(encoded_conn_settings, ci->conn_settings);
	}

	if (ci->translation_dll[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_TRANSLATIONDLL, "", ci->translation_dll, sizeof(ci->translation_dll), ODBC_INI);

	if (ci->translation_option[0] == '\0' || overwrite)
		SQLGetPrivateProfileString(DSN, INI_TRANSLATIONOPTION, "", ci->translation_option, sizeof(ci->translation_option), ODBC_INI);

	if (ci->disallow_premature == 0 || overwrite)
	{
		SQLGetPrivateProfileString(DSN, INI_DISALLOWPREMATURE, "", temp, sizeof(temp), ODBC_INI);
		ci->disallow_premature = atoi(temp);
	}

	if (ci->updatable_cursors == 0 || overwrite)
	{
		SQLGetPrivateProfileString(DSN, INI_UPDATABLECURSORS, "", temp, sizeof(temp), ODBC_INI);
		ci->updatable_cursors = atoi(temp);
	}

	/* Allow override of odbcinst.ini parameters here */
	getCommonDefaults(DSN, ODBC_INI, ci);

	qlog("DSN info: DSN='%s',server='%s',port='%s',dbase='%s',user='%s',passwd='%s'\n",
		 DSN,
		 ci->server,
		 ci->port,
		 ci->database,
		 ci->username,
		 ci->password);
	qlog("          onlyread='%s',protocol='%s',showoid='%s',fakeoidindex='%s',showsystable='%s'\n",
		 ci->onlyread,
		 ci->protocol,
		 ci->show_oid_column,
		 ci->fake_oid_index,
		 ci->show_system_tables);

#ifdef MULTIBYTE
	check_client_encoding(ci->conn_settings);
	qlog("          conn_settings='%s',conn_encoding='%s'\n",
		 ci->conn_settings,
		 check_client_encoding(ci->conn_settings));
#else
	qlog("          conn_settings='%s'\n",
		 ci->conn_settings);
#endif

	qlog("          translation_dll='%s',translation_option='%s'\n",
		 ci->translation_dll,
		 ci->translation_option);
}


/*	This is for datasource based options only */
void
writeDSNinfo(const ConnInfo *ci)
{
	const char *DSN = ci->dsn;
	char		encoded_conn_settings[LARGE_REGISTRY_LEN],
				temp[SMALL_REGISTRY_LEN];

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
								 ci->onlyread,
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

	sprintf(temp, "%d", ci->disallow_premature);
	SQLWritePrivateProfileString(DSN,
								 INI_DISALLOWPREMATURE,
								 temp,
								 ODBC_INI);
	sprintf(temp, "%d", ci->updatable_cursors);
	SQLWritePrivateProfileString(DSN,
								 INI_UPDATABLECURSORS,
								 temp,
								 ODBC_INI);
}


/*
 *	This function reads the ODBCINST.INI portion of
 *	the registry and gets any driver defaults.
 */
void
getCommonDefaults(const char *section, const char *filename, ConnInfo *ci)
{
	char		temp[256];
	GLOBAL_VALUES *comval;

	if (ci)
		comval = &(ci->drivers);
	else
		comval = &globals;
	/* Fetch Count is stored in driver section */
	SQLGetPrivateProfileString(section, INI_FETCH, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
	{
		comval->fetch_max = atoi(temp);
		/* sanity check if using cursors */
		if (comval->fetch_max <= 0)
			comval->fetch_max = FETCH_MAX;
	}
	else if (!ci)
		comval->fetch_max = FETCH_MAX;

	/* Socket Buffersize is stored in driver section */
	SQLGetPrivateProfileString(section, INI_SOCKET, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->socket_buffersize = atoi(temp);
	else if (!ci)
		comval->socket_buffersize = SOCK_BUFFER_SIZE;

	/* Debug is stored in the driver section */
	SQLGetPrivateProfileString(section, INI_DEBUG, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->debug = atoi(temp);
	else if (!ci)
		comval->debug = DEFAULT_DEBUG;

	/* CommLog is stored in the driver section */
	SQLGetPrivateProfileString(section, INI_COMMLOG, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->commlog = atoi(temp);
	else if (!ci)
		comval->commlog = DEFAULT_COMMLOG;

	if (!ci)
		logs_on_off(0, 0, 0);
	/* Optimizer is stored in the driver section only */
	SQLGetPrivateProfileString(section, INI_OPTIMIZER, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->disable_optimizer = atoi(temp);
	else if (!ci)
		comval->disable_optimizer = DEFAULT_OPTIMIZER;

	/* KSQO is stored in the driver section only */
	SQLGetPrivateProfileString(section, INI_KSQO, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->ksqo = atoi(temp);
	else if (!ci)
		comval->ksqo = DEFAULT_KSQO;

	/* Recognize Unique Index is stored in the driver section only */
	SQLGetPrivateProfileString(section, INI_UNIQUEINDEX, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->unique_index = atoi(temp);
	else if (!ci)
		comval->unique_index = DEFAULT_UNIQUEINDEX;


	/* Unknown Sizes is stored in the driver section only */
	SQLGetPrivateProfileString(section, INI_UNKNOWNSIZES, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->unknown_sizes = atoi(temp);
	else if (!ci)
		comval->unknown_sizes = DEFAULT_UNKNOWNSIZES;


	/* Lie about supported functions? */
	SQLGetPrivateProfileString(section, INI_LIE, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->lie = atoi(temp);
	else if (!ci)
		comval->lie = DEFAULT_LIE;

	/* Parse statements */
	SQLGetPrivateProfileString(section, INI_PARSE, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->parse = atoi(temp);
	else if (!ci)
		comval->parse = DEFAULT_PARSE;

	/* SQLCancel calls SQLFreeStmt in Driver Manager */
	SQLGetPrivateProfileString(section, INI_CANCELASFREESTMT, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->cancel_as_freestmt = atoi(temp);
	else if (!ci)
		comval->cancel_as_freestmt = DEFAULT_CANCELASFREESTMT;

	/* UseDeclareFetch is stored in the driver section only */
	SQLGetPrivateProfileString(section, INI_USEDECLAREFETCH, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->use_declarefetch = atoi(temp);
	else if (!ci)
		comval->use_declarefetch = DEFAULT_USEDECLAREFETCH;

	/* Max Varchar Size */
	SQLGetPrivateProfileString(section, INI_MAXVARCHARSIZE, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->max_varchar_size = atoi(temp);
	else if (!ci)
		comval->max_varchar_size = MAX_VARCHAR_SIZE;

	/* Max TextField Size */
	SQLGetPrivateProfileString(section, INI_MAXLONGVARCHARSIZE, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->max_longvarchar_size = atoi(temp);
	else if (!ci)
		comval->max_longvarchar_size = TEXT_FIELD_SIZE;

	/* Text As LongVarchar	*/
	SQLGetPrivateProfileString(section, INI_TEXTASLONGVARCHAR, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->text_as_longvarchar = atoi(temp);
	else if (!ci)
		comval->text_as_longvarchar = DEFAULT_TEXTASLONGVARCHAR;

	/* Unknowns As LongVarchar	*/
	SQLGetPrivateProfileString(section, INI_UNKNOWNSASLONGVARCHAR, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->unknowns_as_longvarchar = atoi(temp);
	else if (!ci)
		comval->unknowns_as_longvarchar = DEFAULT_UNKNOWNSASLONGVARCHAR;

	/* Bools As Char */
	SQLGetPrivateProfileString(section, INI_BOOLSASCHAR, "",
							   temp, sizeof(temp), filename);
	if (temp[0])
		comval->bools_as_char = atoi(temp);
	else if (!ci)
		comval->bools_as_char = DEFAULT_BOOLSASCHAR;

	/* Extra Systable prefixes */

	/*
	 * Use @@@ to distinguish between blank extra prefixes and no key
	 * entry
	 */
	SQLGetPrivateProfileString(section, INI_EXTRASYSTABLEPREFIXES, "@@@",
							   temp, sizeof(temp), filename);
	if (strcmp(temp, "@@@"))
		strcpy(comval->extra_systable_prefixes, temp);
	else if (!ci)
		strcpy(comval->extra_systable_prefixes, DEFAULT_EXTRASYSTABLEPREFIXES);

	mylog("globals.extra_systable_prefixes = '%s'\n", comval->extra_systable_prefixes);


	/* Dont allow override of an override! */
	if (!ci)
	{
		/*
		 * ConnSettings is stored in the driver section and per datasource
		 * for override
		 */
		SQLGetPrivateProfileString(section, INI_CONNSETTINGS, "",
		 comval->conn_settings, sizeof(comval->conn_settings), filename);

		/* Default state for future DSN's Readonly attribute */
		SQLGetPrivateProfileString(section, INI_READONLY, "",
								   temp, sizeof(temp), filename);
		if (temp[0])
			comval->onlyread = atoi(temp);
		else
			comval->onlyread = DEFAULT_READONLY;

		/*
		 * Default state for future DSN's protocol attribute This isn't a
		 * real driver option YET.	This is more intended for
		 * customization from the install.
		 */
		SQLGetPrivateProfileString(section, INI_PROTOCOL, "@@@",
								   temp, sizeof(temp), filename);
		if (strcmp(temp, "@@@"))
			strcpy(comval->protocol, temp);
		else
			strcpy(comval->protocol, DEFAULT_PROTOCOL);
	}
}
