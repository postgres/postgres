#ifndef	_WIN_SETUP_H__
#define	_WIN_SETUP_H__

#ifndef INTFUNC
#define INTFUNC  __stdcall
#endif /* INTFUNC */
#define	MAXDSNAME	(32+1)	/* Max data source name length */
/* Globals */
/* NOTE:  All these are used by the dialog procedures */
typedef struct tagSETUPDLG
{
	HWND		hwndParent;		/* Parent window handle */
	LPCSTR		lpszDrvr;		/* Driver description */
	ConnInfo	ci;
	char		szDSN[MAXDSNAME];		/* Original data source name */
	BOOL		fNewDSN;		/* New data source flag */
	BOOL		fDefault;		/* Default data source flag */

}	SETUPDLG, FAR * LPSETUPDLG;

/* Prototypes */
void INTFUNC CenterDialog(HWND hdlg);
int CALLBACK ConfigDlgProc(HWND hdlg, UINT wMsg, WPARAM wParam, LPARAM lParam);
void INTFUNC ParseAttributes(LPCSTR lpszAttributes, LPSETUPDLG lpsetupdlg);
BOOL INTFUNC SetDSNAttributes(HWND hwnd, LPSETUPDLG lpsetupdlg);

#endif /* _WIN_SETUP_H__ */
