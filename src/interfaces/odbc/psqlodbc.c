
/* Module:          psqlodbc.c
 *
 * Description:     This module contains the main entry point (DllMain) for the library.
 *                  It also contains functions to get and set global variables for the
 *                  driver in the registry.
 *
 * Classes:         n/a
 *
 * API functions:   none
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#include "psqlodbc.h"
#include <winsock.h>
#include <windows.h>
#include <sql.h>
#include <odbcinst.h>

HINSTANCE NEAR s_hModule;               /* Saved module handle. */
GLOBAL_VALUES globals;


/*	This function reads the ODBCINST.INI portion of
	the registry and gets any driver defaults.
*/
void getGlobalDefaults(void)
{
char temp[128];


	//	Fetch Count is stored in driver section
    SQLGetPrivateProfileString(DBMS_NAME, INI_FETCH, "",
                            temp, sizeof(temp), ODBCINST_INI);
	if ( temp[0] )
		globals.fetch_max = atoi(temp);
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
	SQLGetPrivateProfileString(DBMS_NAME, INI_COMMLOG, "0", 
							temp, sizeof(temp), ODBCINST_INI);
	globals.commlog = atoi(temp);


	//	Optimizer is stored in the driver section only (OFF, ON, or ON=x)
	SQLGetPrivateProfileString(DBMS_NAME, INI_OPTIMIZER, "", 
				globals.optimizer, sizeof(globals.optimizer), ODBCINST_INI);


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

	sprintf(tmp, "%d", globals.commlog);
	SQLWritePrivateProfileString(DBMS_NAME,
		INI_COMMLOG, tmp, ODBCINST_INI);
}

/*	This is where the Driver Manager attaches to this Driver */
BOOL WINAPI DllMain(HANDLE hInst, ULONG ul_reason_for_call, LPVOID lpReserved) 
{
WORD wVersionRequested; 
WSADATA wsaData; 

	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		s_hModule = hInst;				/* Save for dialog boxes */

		/*	Load the WinSock Library */
		wVersionRequested = MAKEWORD(1, 1); 

		if ( WSAStartup(wVersionRequested, &wsaData))
			return FALSE;

		/*	Verify that this is the minimum version of WinSock */
		if ( LOBYTE( wsaData.wVersion ) != 1 || 
			HIBYTE( wsaData.wVersion ) != 1 ) { 

			WSACleanup(); 
			return FALSE;
		}

		getGlobalDefaults();
		break;

	case DLL_THREAD_ATTACH:
		break;

	case DLL_PROCESS_DETACH:

		WSACleanup();

		return TRUE;

	case DLL_THREAD_DETACH:
		break;

	default:
		break;
	}

	return TRUE;                                                                
                                                                                
	UNREFERENCED_PARAMETER(lpReserved);                                         
}

/*	This function is used to cause the Driver Manager to
	call functions by number rather than name, which is faster.
	The ordinal value of this function must be 199 to have the
	Driver Manager do this.  Also, the ordinal values of the
	functions must match the value of fFunction in SQLGetFunctions()
*/
RETCODE SQL_API SQLDummyOrdinal(void)
{
	return SQL_SUCCESS;
}


