/* Module:			setup.c
 *
 * Description:		This module contains the setup functions for
 *					adding/modifying a Data Source in the ODBC.INI portion
 *					of the registry.
 *
 * Classes:			n/a
 *
 * API functions:	ConfigDSN
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 *************************************************************************************/

#include  "psqlodbc.h"
#include  "connection.h"
#include  <windows.h>
#include  <windowsx.h>
#include  <odbcinst.h>
#include  <string.h>
#include  <stdlib.h>
#include  "resource.h"
#include  "dlg_specific.h"


#define INTFUNC  __stdcall

extern HINSTANCE NEAR s_hModule;/* Saved module handle. */
extern GLOBAL_VALUES globals;

/* Constants --------------------------------------------------------------- */
#define MIN(x,y)	  ((x) < (y) ? (x) : (y))

#ifdef WIN32
#define MAXPGPATH		(255+1)
#endif

#define MAXKEYLEN		(15+1)	/* Max keyword length */
#define MAXDESC			(255+1) /* Max description length */
#define MAXDSNAME		(32+1)	/* Max data source name length */


/* Globals ----------------------------------------------------------------- */
/* NOTE:  All these are used by the dialog procedures */
typedef struct tagSETUPDLG
{
	HWND		hwndParent;		/* Parent window handle */
	LPCSTR		lpszDrvr;		/* Driver description */
	ConnInfo	ci;
	char		szDSN[MAXDSNAME];		/* Original data source name */
	BOOL		fNewDSN;		/* New data source flag */
	BOOL		fDefault;		/* Default data source flag */
}			SETUPDLG, FAR * LPSETUPDLG;



/* Prototypes -------------------------------------------------------------- */
void INTFUNC CenterDialog(HWND hdlg);
int CALLBACK ConfigDlgProc(HWND hdlg, WORD wMsg, WPARAM wParam, LPARAM lParam);
void INTFUNC ParseAttributes(LPCSTR lpszAttributes, LPSETUPDLG lpsetupdlg);
BOOL INTFUNC SetDSNAttributes(HWND hwnd, LPSETUPDLG lpsetupdlg);


/* ConfigDSN ---------------------------------------------------------------
  Description:	ODBC Setup entry point
				This entry point is called by the ODBC Installer
				(see file header for more details)
  Input		 :	hwnd ----------- Parent window handle
				fRequest ------- Request type (i.e., add, config, or remove)
				lpszDriver ----- Driver name
				lpszAttributes - data source attribute string
  Output	 :	TRUE success, FALSE otherwise
--------------------------------------------------------------------------*/

BOOL		CALLBACK
ConfigDSN(HWND hwnd,
		  WORD fRequest,
		  LPCSTR lpszDriver,
		  LPCSTR lpszAttributes)
{
	BOOL		fSuccess;		/* Success/fail flag */
	GLOBALHANDLE hglbAttr;
	LPSETUPDLG	lpsetupdlg;


	/* Allocate attribute array */
	hglbAttr = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(SETUPDLG));
	if (!hglbAttr)
		return FALSE;
	lpsetupdlg = (LPSETUPDLG) GlobalLock(hglbAttr);

	/* Parse attribute string */
	if (lpszAttributes)
		ParseAttributes(lpszAttributes, lpsetupdlg);

	/* Save original data source name */
	if (lpsetupdlg->ci.dsn[0])
		lstrcpy(lpsetupdlg->szDSN, lpsetupdlg->ci.dsn);
	else
		lpsetupdlg->szDSN[0] = '\0';

	/* Remove data source */
	if (ODBC_REMOVE_DSN == fRequest)
	{
		/* Fail if no data source name was supplied */
		if (!lpsetupdlg->ci.dsn[0])
			fSuccess = FALSE;

		/* Otherwise remove data source from ODBC.INI */
		else
			fSuccess = SQLRemoveDSNFromIni(lpsetupdlg->ci.dsn);
	}

	/* Add or Configure data source */
	else
	{
		/* Save passed variables for global access (e.g., dialog access) */
		lpsetupdlg->hwndParent = hwnd;
		lpsetupdlg->lpszDrvr = lpszDriver;
		lpsetupdlg->fNewDSN = (ODBC_ADD_DSN == fRequest);
		lpsetupdlg->fDefault = !lstrcmpi(lpsetupdlg->ci.dsn, INI_DSN);

		/*
		 * Display the appropriate dialog (if parent window handle
		 * supplied)
		 */
		if (hwnd)
		{
			/* Display dialog(s) */
			fSuccess = (IDOK == DialogBoxParam(s_hModule,
											 MAKEINTRESOURCE(DLG_CONFIG),
											   hwnd,
											   ConfigDlgProc,
											 (LONG) (LPSTR) lpsetupdlg));
		}

		else if (lpsetupdlg->ci.dsn[0])
			fSuccess = SetDSNAttributes(hwnd, lpsetupdlg);
		else
			fSuccess = FALSE;
	}

	GlobalUnlock(hglbAttr);
	GlobalFree(hglbAttr);

	return fSuccess;
}


/* CenterDialog ------------------------------------------------------------
		Description:  Center the dialog over the frame window
		Input	   :  hdlg -- Dialog window handle
		Output	   :  None
--------------------------------------------------------------------------*/
void		INTFUNC
CenterDialog(HWND hdlg)
{
	HWND		hwndFrame;
	RECT		rcDlg,
				rcScr,
				rcFrame;
	int			cx,
				cy;

	hwndFrame = GetParent(hdlg);

	GetWindowRect(hdlg, &rcDlg);
	cx = rcDlg.right - rcDlg.left;
	cy = rcDlg.bottom - rcDlg.top;

	GetClientRect(hwndFrame, &rcFrame);
	ClientToScreen(hwndFrame, (LPPOINT) (&rcFrame.left));
	ClientToScreen(hwndFrame, (LPPOINT) (&rcFrame.right));
	rcDlg.top = rcFrame.top + (((rcFrame.bottom - rcFrame.top) - cy) >> 1);
	rcDlg.left = rcFrame.left + (((rcFrame.right - rcFrame.left) - cx) >> 1);
	rcDlg.bottom = rcDlg.top + cy;
	rcDlg.right = rcDlg.left + cx;

	GetWindowRect(GetDesktopWindow(), &rcScr);
	if (rcDlg.bottom > rcScr.bottom)
	{
		rcDlg.bottom = rcScr.bottom;
		rcDlg.top = rcDlg.bottom - cy;
	}
	if (rcDlg.right > rcScr.right)
	{
		rcDlg.right = rcScr.right;
		rcDlg.left = rcDlg.right - cx;
	}

	if (rcDlg.left < 0)
		rcDlg.left = 0;
	if (rcDlg.top < 0)
		rcDlg.top = 0;

	MoveWindow(hdlg, rcDlg.left, rcDlg.top, cx, cy, TRUE);
	return;
}

/* ConfigDlgProc -----------------------------------------------------------
  Description:	Manage add data source name dialog
  Input		 :	hdlg --- Dialog window handle
				wMsg --- Message
				wParam - Message parameter
				lParam - Message parameter
  Output	 :	TRUE if message processed, FALSE otherwise
--------------------------------------------------------------------------*/


int			CALLBACK
ConfigDlgProc(HWND hdlg,
			  WORD wMsg,
			  WPARAM wParam,
			  LPARAM lParam)
{
	switch (wMsg)
	{
			/* Initialize the dialog */
			case WM_INITDIALOG:
			{
				LPSETUPDLG	lpsetupdlg = (LPSETUPDLG) lParam;
				ConnInfo   *ci = &lpsetupdlg->ci;

				/* Hide the driver connect message */
				ShowWindow(GetDlgItem(hdlg, DRV_MSG_LABEL), SW_HIDE);

				SetWindowLong(hdlg, DWL_USER, lParam);
				CenterDialog(hdlg);		/* Center dialog */

				/*
				 * NOTE: Values supplied in the attribute string will
				 * always
				 */
				/* override settings in ODBC.INI */

				/* Get the rest of the common attributes */
				getDSNinfo(ci, CONN_DONT_OVERWRITE);

				/* Fill in any defaults */
				getDSNdefaults(ci);


				/* Initialize dialog fields */
				SetDlgStuff(hdlg, ci);


				if (lpsetupdlg->fDefault)
				{
					EnableWindow(GetDlgItem(hdlg, IDC_DSNAME), FALSE);
					EnableWindow(GetDlgItem(hdlg, IDC_DSNAMETEXT), FALSE);
				}
				else
					SendDlgItemMessage(hdlg, IDC_DSNAME,
							 EM_LIMITTEXT, (WPARAM) (MAXDSNAME - 1), 0L);

				SendDlgItemMessage(hdlg, IDC_DESC,
							   EM_LIMITTEXT, (WPARAM) (MAXDESC - 1), 0L);
				return TRUE;	/* Focus was not set */
			}


			/* Process buttons */
		case WM_COMMAND:

			switch (GET_WM_COMMAND_ID(wParam, lParam))
			{
					/*
					 * Ensure the OK button is enabled only when a data
					 * source name
					 */
					/* is entered */
				case IDC_DSNAME:
					if (GET_WM_COMMAND_CMD(wParam, lParam) == EN_CHANGE)
					{
						char		szItem[MAXDSNAME];	/* Edit control text */

						/* Enable/disable the OK button */
						EnableWindow(GetDlgItem(hdlg, IDOK),
									 GetDlgItemText(hdlg, IDC_DSNAME,
												szItem, sizeof(szItem)));

						return TRUE;
					}
					break;

					/* Accept results */
				case IDOK:
					{
						LPSETUPDLG	lpsetupdlg;

						lpsetupdlg = (LPSETUPDLG) GetWindowLong(hdlg, DWL_USER);
						/* Retrieve dialog values */
						if (!lpsetupdlg->fDefault)
							GetDlgItemText(hdlg, IDC_DSNAME,
										   lpsetupdlg->ci.dsn,
										   sizeof(lpsetupdlg->ci.dsn));


						/* Get Dialog Values */
						GetDlgStuff(hdlg, &lpsetupdlg->ci);

						/* Update ODBC.INI */
						SetDSNAttributes(hdlg, lpsetupdlg);
					}

					/* Return to caller */
				case IDCANCEL:
					EndDialog(hdlg, wParam);
					return TRUE;

				case IDC_DRIVER:

					DialogBoxParam(s_hModule, MAKEINTRESOURCE(DLG_OPTIONS_DRV),
								hdlg, driver_optionsProc, (LPARAM) NULL);

					return TRUE;

				case IDC_DATASOURCE:
					{
						LPSETUPDLG	lpsetupdlg;

						lpsetupdlg = (LPSETUPDLG) GetWindowLong(hdlg, DWL_USER);

						DialogBoxParam(s_hModule, MAKEINTRESOURCE(DLG_OPTIONS_DS),
						hdlg, ds_optionsProc, (LPARAM) & lpsetupdlg->ci);

						return TRUE;
					}
			}

			break;
	}

	/* Message not processed */
	return FALSE;
}


/* ParseAttributes ---------------------------------------------------------
  Description:	Parse attribute string moving values into the aAttr array
  Input		 :	lpszAttributes - Pointer to attribute string
  Output	 :	None (global aAttr normally updated)
--------------------------------------------------------------------------*/
void		INTFUNC
ParseAttributes(LPCSTR lpszAttributes, LPSETUPDLG lpsetupdlg)
{
	LPCSTR		lpsz;
	LPCSTR		lpszStart;
	char		aszKey[MAXKEYLEN];
	int			cbKey;
	char		value[MAXPGPATH];

	memset(&lpsetupdlg->ci, 0, sizeof(ConnInfo));

	for (lpsz = lpszAttributes; *lpsz; lpsz++)
	{							/* Extract key name (e.g., DSN), it must
								 * be terminated by an equals */
		lpszStart = lpsz;
		for (;; lpsz++)
		{
			if (!*lpsz)
				return;			/* No key was found */
			else if (*lpsz == '=')
				break;			/* Valid key found */
		}
		/* Determine the key's index in the key table (-1 if not found) */
		cbKey = lpsz - lpszStart;
		if (cbKey < sizeof(aszKey))
		{
			_fmemcpy(aszKey, lpszStart, cbKey);
			aszKey[cbKey] = '\0';
		}

		/* Locate end of key value */
		lpszStart = ++lpsz;
		for (; *lpsz; lpsz++);


		/* lpsetupdlg->aAttr[iElement].fSupplied = TRUE; */
		_fmemcpy(value, lpszStart, MIN(lpsz - lpszStart + 1, MAXPGPATH));

		mylog("aszKey='%s', value='%s'\n", aszKey, value);

		/* Copy the appropriate value to the conninfo  */
		copyAttributes(&lpsetupdlg->ci, aszKey, value);
	}
	return;
}


/* SetDSNAttributes --------------------------------------------------------
  Description:	Write data source attributes to ODBC.INI
  Input		 :	hwnd - Parent window handle (plus globals)
  Output	 :	TRUE if successful, FALSE otherwise
--------------------------------------------------------------------------*/

BOOL		INTFUNC
SetDSNAttributes(HWND hwndParent, LPSETUPDLG lpsetupdlg)
{
	LPCSTR		lpszDSN;		/* Pointer to data source name */

	lpszDSN = lpsetupdlg->ci.dsn;

	/* Validate arguments */
	if (lpsetupdlg->fNewDSN && !*lpsetupdlg->ci.dsn)
		return FALSE;

	/* Write the data source name */
	if (!SQLWriteDSNToIni(lpszDSN, lpsetupdlg->lpszDrvr))
	{
		if (hwndParent)
		{
			char		szBuf[MAXPGPATH];
			char		szMsg[MAXPGPATH];

			LoadString(s_hModule, IDS_BADDSN, szBuf, sizeof(szBuf));
			wsprintf(szMsg, szBuf, lpszDSN);
			LoadString(s_hModule, IDS_MSGTITLE, szBuf, sizeof(szBuf));
			MessageBox(hwndParent, szMsg, szBuf, MB_ICONEXCLAMATION | MB_OK);
		}
		return FALSE;
	}


	/* Update ODBC.INI */
	writeDSNinfo(&lpsetupdlg->ci);


	/* If the data source name has changed, remove the old name */
	if (lstrcmpi(lpsetupdlg->szDSN, lpsetupdlg->ci.dsn))
		SQLRemoveDSNFromIni(lpsetupdlg->szDSN);
	return TRUE;
}
