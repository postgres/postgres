/*-------------------------------------------------------------------------
 *
 * validator.c
 *	  Test module for serverside OAuth token validation callbacks
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/modules/oauth_validator/validator.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/oauth.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

static void validator_startup(ValidatorModuleState *state);
static void validator_shutdown(ValidatorModuleState *state);
static bool validate_token(const ValidatorModuleState *state,
						   const char *token,
						   const char *role,
						   ValidatorModuleResult *res);

/* Callback implementations (exercise all three) */
static const OAuthValidatorCallbacks validator_callbacks = {
	PG_OAUTH_VALIDATOR_MAGIC,

	.startup_cb = validator_startup,
	.shutdown_cb = validator_shutdown,
	.validate_cb = validate_token
};

/* GUCs */
static char *authn_id = NULL;
static bool authorize_tokens = true;
static char *error_detail = NULL;
static bool internal_error = false;
static bool invalid_hba = false;

/* HBA options */
static const char *hba_opts[] = {
	"authn_id",					/* overrides the default authn_id */
	"log",						/* logs an arbitrary string */
};

/*---
 * Extension entry point. Sets up GUCs for use by tests:
 *
 * - oauth_validator.authn_id	Sets the user identifier to return during token
 *								validation. Defaults to the username in the
 *								startup packet, or the validator.authn_id HBA
 *								option if it is set.
 *
 * - oauth_validator.authorize_tokens
 *								Sets whether to successfully validate incoming
 *								tokens. Defaults to true.
 *
 * - oauth_validator.error_detail
 *                              Sets an error message to be included as a
 *                              DETAIL on failure.
 *
 * - oauth_validator.internal_error
 *                              Reports an internal error to the server.
 */
void
_PG_init(void)
{
	DefineCustomStringVariable("oauth_validator.authn_id",
							   "Authenticated identity to use for future connections",
							   NULL,
							   &authn_id,
							   NULL,
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);
	DefineCustomBoolVariable("oauth_validator.authorize_tokens",
							 "Should tokens be marked valid?",
							 NULL,
							 &authorize_tokens,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
	DefineCustomStringVariable("oauth_validator.error_detail",
							   "Error message to print during failures",
							   NULL,
							   &error_detail,
							   NULL,
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);
	DefineCustomBoolVariable("oauth_validator.internal_error",
							 "Should the validator report an internal error?",
							 NULL,
							 &internal_error,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("oauth_validator.invalid_hba",
							 "Should the validator register an invalid option?",
							 NULL,
							 &invalid_hba,
							 false,
							 PGC_SIGHUP,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("oauth_validator");
}

/*
 * Validator module entry point.
 */
const OAuthValidatorCallbacks *
_PG_oauth_validator_module_init(void)
{
	return &validator_callbacks;
}

#define PRIVATE_COOKIE ((void *) 13579)

/*
 * Startup callback, to set up private data for the validator.
 */
static void
validator_startup(ValidatorModuleState *state)
{
	/*
	 * Make sure the server is correctly setting sversion. (Real modules
	 * should not do this; it would defeat upgrade compatibility.)
	 */
	if (state->sversion != PG_VERSION_NUM)
		elog(ERROR, "oauth_validator: sversion set to %d", state->sversion);

	/*
	 * Test the behavior of custom HBA options. Registered options should not
	 * be retrievable during startup (we want to discourage modules from
	 * relying on the relative order of client connections and the
	 * startup_cb).
	 */
	RegisterOAuthHBAOptions(state, lengthof(hba_opts), hba_opts);
	for (int i = 0; i < lengthof(hba_opts); i++)
	{
		if (GetOAuthHBAOption(state, hba_opts[i]))
			elog(ERROR,
				 "oauth_validator: GetOAuthValidatorOption(\"%s\") was non-NULL during startup_cb",
				 hba_opts[i]);
	}

	if (invalid_hba)
	{
		/* Register a bad option, which should print a WARNING to the logs. */
		const char *invalid = "bad option name";

		RegisterOAuthHBAOptions(state, 1, &invalid);
	}

	state->private_data = PRIVATE_COOKIE;
}

/*
 * Shutdown callback, to tear down the validator.
 */
static void
validator_shutdown(ValidatorModuleState *state)
{
	/* Check to make sure our private state still exists. */
	if (state->private_data != PRIVATE_COOKIE)
		elog(PANIC, "oauth_validator: private state cookie changed to %p in shutdown",
			 state->private_data);
}

/*
 * Validator implementation. Logs the incoming data and authorizes the token by
 * default; the behavior can be modified via the module's GUC and HBA settings.
 */
static bool
validate_token(const ValidatorModuleState *state,
			   const char *token, const char *role,
			   ValidatorModuleResult *res)
{
	/* Check to make sure our private state still exists. */
	if (state->private_data != PRIVATE_COOKIE)
		elog(ERROR, "oauth_validator: private state cookie changed to %p in validate",
			 state->private_data);

	if (GetOAuthHBAOption(state, "log"))
		elog(LOG, "%s", GetOAuthHBAOption(state, "log"));

	elog(LOG, "oauth_validator: token=\"%s\", role=\"%s\"", token, role);
	elog(LOG, "oauth_validator: issuer=\"%s\", scope=\"%s\"",
		 MyProcPort->hba->oauth_issuer,
		 MyProcPort->hba->oauth_scope);

	res->error_detail = error_detail;	/* only relevant for failures */
	if (internal_error)
		return false;

	res->authorized = authorize_tokens;
	if (authn_id)
		res->authn_id = pstrdup(authn_id);
	else if (GetOAuthHBAOption(state, "authn_id"))
		res->authn_id = pstrdup(GetOAuthHBAOption(state, "authn_id"));
	else
		res->authn_id = pstrdup(role);

	return true;
}
