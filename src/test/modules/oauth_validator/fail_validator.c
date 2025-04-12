/*-------------------------------------------------------------------------
 *
 * fail_validator.c
 *	  Test module for serverside OAuth token validation callbacks, which is
 *	  guaranteed to always fail in the validation callback
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/modules/oauth_validator/fail_validator.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/oauth.h"

PG_MODULE_MAGIC;

static bool fail_token(const ValidatorModuleState *state,
					   const char *token,
					   const char *role,
					   ValidatorModuleResult *res);

/* Callback implementations (we only need the main one) */
static const OAuthValidatorCallbacks validator_callbacks = {
	PG_OAUTH_VALIDATOR_MAGIC,

	.validate_cb = fail_token,
};

const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
	return &validator_callbacks;
}

static bool
fail_token(const ValidatorModuleState *state,
		   const char *token, const char *role,
		   ValidatorModuleResult *res)
{
	elog(FATAL, "fail_validator: sentinel error");
	pg_unreachable();
}
