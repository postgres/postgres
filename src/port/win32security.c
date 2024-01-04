/*-------------------------------------------------------------------------
 *
 * win32security.c
 *	  Microsoft Windows Win32 Security Support Functions
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32security.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

static void log_error(const char *fmt,...) pg_attribute_printf(1, 2);


/*
 * Utility wrapper for frontend and backend when reporting an error
 * message.
 */
static void
log_error(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
#ifndef FRONTEND
	write_stderr(fmt, ap);
#else
	fprintf(stderr, fmt, ap);
#endif
	va_end(ap);
}

/*
 * Returns nonzero if the current user has administrative privileges,
 * or zero if not.
 *
 * Note: this cannot use ereport() because it's called too early during
 * startup.
 */
int
pgwin32_is_admin(void)
{
	PSID		AdministratorsSid;
	PSID		PowerUsersSid;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	BOOL		IsAdministrators;
	BOOL		IsPowerUsers;

	if (!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID,
								  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
								  0, &AdministratorsSid))
	{
		log_error(_("could not get SID for Administrators group: error code %lu\n"),
				  GetLastError());
		exit(1);
	}

	if (!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID,
								  DOMAIN_ALIAS_RID_POWER_USERS, 0, 0, 0, 0, 0,
								  0, &PowerUsersSid))
	{
		log_error(_("could not get SID for PowerUsers group: error code %lu\n"),
				  GetLastError());
		exit(1);
	}

	if (!CheckTokenMembership(NULL, AdministratorsSid, &IsAdministrators) ||
		!CheckTokenMembership(NULL, PowerUsersSid, &IsPowerUsers))
	{
		log_error(_("could not check access token membership: error code %lu\n"),
				  GetLastError());
		exit(1);
	}

	FreeSid(AdministratorsSid);
	FreeSid(PowerUsersSid);

	if (IsAdministrators || IsPowerUsers)
		return 1;
	else
		return 0;
}

/*
 * We consider ourselves running as a service if one of the following is
 * true:
 *
 * 1) Standard error is not valid (always the case for services, and pg_ctl
 *	  running as a service "passes" that down to postgres,
 *	  c.f. CreateRestrictedProcess())
 * 2) We are running as LocalSystem (only used by services)
 * 3) Our token contains SECURITY_SERVICE_RID (automatically added to the
 *	  process token by the SCM when starting a service)
 *
 * The check for LocalSystem is needed, because surprisingly, if a service
 * is running as LocalSystem, it does not have SECURITY_SERVICE_RID in its
 * process token.
 *
 * Return values:
 *	 0 = Not service
 *	 1 = Service
 *	-1 = Error
 *
 * Note: we can't report errors via either ereport (we're called too early
 * in the backend) or write_stderr (because that calls this).  We are
 * therefore reduced to writing directly on stderr, which sucks, but we
 * have few alternatives.
 */
int
pgwin32_is_service(void)
{
	static int	_is_service = -1;
	BOOL		IsMember;
	PSID		ServiceSid;
	PSID		LocalSystemSid;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	HANDLE		stderr_handle;

	/* Only check the first time */
	if (_is_service != -1)
		return _is_service;

	/* Check if standard error is not valid */
	stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
	if (stderr_handle != INVALID_HANDLE_VALUE && stderr_handle != NULL)
	{
		_is_service = 0;
		return _is_service;
	}

	/* Check if running as LocalSystem */
	if (!AllocateAndInitializeSid(&NtAuthority, 1,
								  SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
								  &LocalSystemSid))
	{
		fprintf(stderr, "could not get SID for local system account\n");
		return -1;
	}

	if (!CheckTokenMembership(NULL, LocalSystemSid, &IsMember))
	{
		fprintf(stderr, "could not check access token membership: error code %lu\n",
				GetLastError());
		FreeSid(LocalSystemSid);
		return -1;
	}
	FreeSid(LocalSystemSid);

	if (IsMember)
	{
		_is_service = 1;
		return _is_service;
	}

	/* Check for service group membership */
	if (!AllocateAndInitializeSid(&NtAuthority, 1,
								  SECURITY_SERVICE_RID, 0, 0, 0, 0, 0, 0, 0,
								  &ServiceSid))
	{
		fprintf(stderr, "could not get SID for service group: error code %lu\n",
				GetLastError());
		return -1;
	}

	if (!CheckTokenMembership(NULL, ServiceSid, &IsMember))
	{
		fprintf(stderr, "could not check access token membership: error code %lu\n",
				GetLastError());
		FreeSid(ServiceSid);
		return -1;
	}
	FreeSid(ServiceSid);

	if (IsMember)
		_is_service = 1;
	else
		_is_service = 0;

	return _is_service;
}
