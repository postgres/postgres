/*-------------------------------------------------------------------------
 *
 * oauthtest.c
 *	  Test module for serverside OAuth token validation callbacks
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/python/server/oauthtest.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/oauth.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

static void test_startup(ValidatorModuleState *state);
static void test_shutdown(ValidatorModuleState *state);
static bool test_validate(const ValidatorModuleState *state,
						  const char *token,
						  const char *role,
						  ValidatorModuleResult *result);

static const OAuthValidatorCallbacks callbacks = {
	PG_OAUTH_VALIDATOR_MAGIC,

	.startup_cb = test_startup,
	.shutdown_cb = test_shutdown,
	.validate_cb = test_validate,
};

static char *expected_bearer = "";
static bool set_authn_id = false;
static char *authn_id = "";
static bool reflect_role = false;

void
_PG_init(void)
{
	DefineCustomStringVariable("oauthtest.expected_bearer",
							   "Expected Bearer token for future connections",
							   NULL,
							   &expected_bearer,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("oauthtest.set_authn_id",
							 "Whether to set an authenticated identity",
							 NULL,
							 &set_authn_id,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
	DefineCustomStringVariable("oauthtest.authn_id",
							   "Authenticated identity to use for future connections",
							   NULL,
							   &authn_id,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("oauthtest.reflect_role",
							 "Ignore the bearer token; use the requested role as the authn_id",
							 NULL,
							 &reflect_role,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("oauthtest");
}

const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
	return &callbacks;
}

static void
test_startup(ValidatorModuleState *state)
{
}

static void
test_shutdown(ValidatorModuleState *state)
{
}

static bool
test_validate(const ValidatorModuleState *state,
			  const char *token, const char *role,
			  ValidatorModuleResult *res)
{
	if (reflect_role)
	{
		res->authorized = true;
		res->authn_id = pstrdup(role);
	}
	else
	{
		if (*expected_bearer && strcmp(token, expected_bearer) == 0)
			res->authorized = true;
		if (set_authn_id)
			res->authn_id = pstrdup(authn_id);
	}

	return true;
}
