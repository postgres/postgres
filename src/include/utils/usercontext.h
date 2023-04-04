/*-------------------------------------------------------------------------
 *
 * usercontext.h
 *	  Convenience functions for running code as a different database user.
 *
 *-------------------------------------------------------------------------
 */
#ifndef USERCONTEXT_H
#define USERCONTEXT_H

/*
 * When temporarily changing to run as a different user, this structure
 * holds the details needed to restore the original state.
 */
typedef struct UserContext
{
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
} UserContext;

/* Function prototypes. */
extern void SwitchToUntrustedUser(Oid userid, UserContext *context);
extern void RestoreUserContext(UserContext *context);

#endif							/* USERCONTEXT_H */
