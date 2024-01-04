/*-------------------------------------------------------------------------
 *
 * usercontext.c
 *	  Convenience functions for running code as a different database user.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/usercontext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/usercontext.h"

/*
 * Temporarily switch to a new user ID.
 *
 * If the current user doesn't have permission to SET ROLE to the new user,
 * an ERROR occurs.
 *
 * If the new user doesn't have permission to SET ROLE to the current user,
 * SECURITY_RESTRICTED_OPERATION is imposed and a new GUC nest level is
 * created so that any settings changes can be rolled back.
 */
void
SwitchToUntrustedUser(Oid userid, UserContext *context)
{
	/* Get the current user ID and security context. */
	GetUserIdAndSecContext(&context->save_userid,
						   &context->save_sec_context);

	/* Check that we have sufficient privileges to assume the target role. */
	if (!member_can_set_role(context->save_userid, userid))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("role \"%s\" cannot SET ROLE to \"%s\"",
						GetUserNameFromId(context->save_userid, false),
						GetUserNameFromId(userid, false))));

	/*
	 * Try to prevent the user to which we're switching from assuming the
	 * privileges of the current user, unless they can SET ROLE to that user
	 * anyway.
	 */
	if (member_can_set_role(userid, context->save_userid))
	{
		/*
		 * Each user can SET ROLE to the other, so there's no point in
		 * imposing any security restrictions. Just let the user do whatever
		 * they want.
		 */
		SetUserIdAndSecContext(userid, context->save_sec_context);
		context->save_nestlevel = -1;
	}
	else
	{
		int			sec_context = context->save_sec_context;

		/*
		 * This user can SET ROLE to the target user, but not the other way
		 * around, so protect ourselves against the target user by setting
		 * SECURITY_RESTRICTED_OPERATION to prevent certain changes to the
		 * session state. Also set up a new GUC nest level, so that we can
		 * roll back any GUC changes that may be made by code running as the
		 * target user, inasmuch as they could be malicious.
		 */
		sec_context |= SECURITY_RESTRICTED_OPERATION;
		SetUserIdAndSecContext(userid, sec_context);
		context->save_nestlevel = NewGUCNestLevel();
	}
}

/*
 * Switch back to the original user ID.
 *
 * If we created a new GUC nest level, also roll back any changes that were
 * made within it.
 */
void
RestoreUserContext(UserContext *context)
{
	if (context->save_nestlevel != -1)
		AtEOXact_GUC(false, context->save_nestlevel);
	SetUserIdAndSecContext(context->save_userid, context->save_sec_context);
}
