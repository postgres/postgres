/*-------------------------------------------------------------------------
 *
 * magic_validator.c
 *	  Test module for serverside OAuth token validation callbacks, which
 *	  should fail due to using the wrong PG_OAUTH_VALIDATOR_MAGIC marker
 *	  and thus the wrong ABI version
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/modules/oauth_validator/magic_validator.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/oauth.h"

PG_MODULE_MAGIC;

static bool validate_token(const ValidatorModuleState *state,
						   const char *token,
						   const char *role,
						   ValidatorModuleResult *res);

/* Callback implementations (we only need the main one) */
static const OAuthValidatorCallbacks validator_callbacks = {
	0xdeadbeef,

	.validate_cb = validate_token,
};

const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
	return &validator_callbacks;
}

static bool
validate_token(const ValidatorModuleState *state,
			   const char *token, const char *role,
			   ValidatorModuleResult *res)
{
	elog(FATAL, "magic_validator: this should be unreachable");
	pg_unreachable();
}
