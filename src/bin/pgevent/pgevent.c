/*-------------------------------------------------------------------------
 *
 * pgevent.c
 *		Defines the entry point for pgevent dll.
 *		The DLL defines event source for backend
 *
 *
 * IDENTIFICATION
 *	  src/bin/pgevent/pgevent.c
 *
 *-------------------------------------------------------------------------
 */


#include "postgres_fe.h"

#include <windows.h>
#include <olectl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Global variables */
HANDLE		g_module = NULL;	/* hModule of DLL */

/*
 * The event source is stored as a registry key.
 * The maximum length of a registry key is 255 characters.
 * http://msdn.microsoft.com/en-us/library/ms724872(v=vs.85).aspx
 */
char		event_source[256] = DEFAULT_EVENT_SOURCE;

/* Prototypes */
HRESULT		DllInstall(BOOL bInstall, LPCWSTR pszCmdLine);
STDAPI		DllRegisterServer(void);
STDAPI		DllUnregisterServer(void);
BOOL WINAPI DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);

/*
 * DllInstall --- Passes the command line argument to DLL
 */

HRESULT
DllInstall(BOOL bInstall,
		   LPCWSTR pszCmdLine)
{
	if (pszCmdLine && *pszCmdLine != '\0')
		wcstombs(event_source, pszCmdLine, sizeof(event_source));

	/*
	 * This is an ugly hack due to the strange behavior of "regsvr32 /i".
	 *
	 * When installing, regsvr32 calls DllRegisterServer before DllInstall.
	 * When uninstalling (i.e. "regsvr32 /u /i"), on the other hand, regsvr32
	 * calls DllInstall and then DllUnregisterServer as expected.
	 *
	 * This strange behavior forces us to specify -n (i.e. "regsvr32 /n /i").
	 * Without -n, DllRegisterServer called before DllInstall would mistakenly
	 * overwrite the default "PostgreSQL" event source registration.
	 */
	if (bInstall)
		DllRegisterServer();
	return S_OK;
}

/*
 * DllRegisterServer --- Instructs DLL to create its registry entries
 */

STDAPI
DllRegisterServer(void)
{
	HKEY		key;
	DWORD		data;
	char		buffer[_MAX_PATH];
	char		key_name[400];

	/* Set the name of DLL full path name. */
	if (!GetModuleFileName((HMODULE) g_module, buffer, sizeof(buffer)))
	{
		MessageBox(NULL, "Could not retrieve DLL filename", "PostgreSQL error", MB_OK | MB_ICONSTOP);
		return SELFREG_E_TYPELIB;
	}

	/*
	 * Add PostgreSQL source name as a subkey under the Application key in the
	 * EventLog registry key.
	 */
	_snprintf(key_name, sizeof(key_name),
			"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s",
			  event_source);
	if (RegCreateKey(HKEY_LOCAL_MACHINE, key_name, &key))
	{
		MessageBox(NULL, "Could not create the registry key.", "PostgreSQL error", MB_OK | MB_ICONSTOP);
		return SELFREG_E_TYPELIB;
	}

	/* Add the name to the EventMessageFile subkey. */
	if (RegSetValueEx(key,
					  "EventMessageFile",
					  0,
					  REG_EXPAND_SZ,
					  (LPBYTE) buffer,
					  strlen(buffer) + 1))
	{
		MessageBox(NULL, "Could not set the event message file.", "PostgreSQL error", MB_OK | MB_ICONSTOP);
		return SELFREG_E_TYPELIB;
	}

	/* Set the supported event types in the TypesSupported subkey. */
	data = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;

	if (RegSetValueEx(key,
					  "TypesSupported",
					  0,
					  REG_DWORD,
					  (LPBYTE) &data,
					  sizeof(DWORD)))
	{
		MessageBox(NULL, "Could not set the supported types.", "PostgreSQL error", MB_OK | MB_ICONSTOP);
		return SELFREG_E_TYPELIB;
	}

	RegCloseKey(key);
	return S_OK;
}

/*
 * DllUnregisterServer --- Instructs DLL to remove only those entries created through DllRegisterServer
 */

STDAPI
DllUnregisterServer(void)
{
	char		key_name[400];

	/*
	 * Remove PostgreSQL source name as a subkey under the Application key in
	 * the EventLog registry key.
	 */

	_snprintf(key_name, sizeof(key_name),
			"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s",
			  event_source);
	if (RegDeleteKey(HKEY_LOCAL_MACHINE, key_name))
	{
		MessageBox(NULL, "Could not delete the registry key.", "PostgreSQL error", MB_OK | MB_ICONSTOP);
		return SELFREG_E_TYPELIB;
	}
	return S_OK;
}

/*
 * DllMain --- is an optional entry point into a DLL.
 */

BOOL		WINAPI
DllMain(HANDLE hModule,
		DWORD ul_reason_for_call,
		LPVOID lpReserved
)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
		g_module = hModule;
	return TRUE;
}
