/*-------------------------------------------------------------------------
 *
 * oauth.h
 *	  Interface to libpq/auth-oauth.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/oauth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OAUTH_H
#define PG_OAUTH_H

#include "libpq/libpq-be.h"
#include "libpq/sasl.h"

extern PGDLLIMPORT char *oauth_validator_libraries_string;

typedef struct ValidatorModuleState
{
	/* Holds the server's PG_VERSION_NUM. Reserved for future extensibility. */
	int			sversion;

	/*
	 * Private data pointer for use by a validator module. This can be used to
	 * store state for the module that will be passed to each of its
	 * callbacks.
	 */
	void	   *private_data;
} ValidatorModuleState;

typedef struct ValidatorModuleResult
{
	/*
	 * Should be set to true if the token carries sufficient permissions for
	 * the bearer to connect.
	 */
	bool		authorized;

	/*
	 * If the token authenticates the user, this should be set to a palloc'd
	 * string containing the SYSTEM_USER to use for HBA mapping. Consider
	 * setting this even if result->authorized is false so that DBAs may use
	 * the logs to match end users to token failures.
	 *
	 * This is required if the module is not configured for ident mapping
	 * delegation. See the validator module documentation for details.
	 */
	char	   *authn_id;
} ValidatorModuleResult;

/*
 * Validator module callbacks
 *
 * These callback functions should be defined by validator modules and returned
 * via _PG_oauth_validator_module_init().  ValidatorValidateCB is the only
 * required callback. For more information about the purpose of each callback,
 * refer to the OAuth validator modules documentation.
 */
typedef void (*ValidatorStartupCB) (ValidatorModuleState *state);
typedef void (*ValidatorShutdownCB) (ValidatorModuleState *state);
typedef bool (*ValidatorValidateCB) (const ValidatorModuleState *state,
									 const char *token, const char *role,
									 ValidatorModuleResult *result);

/*
 * Identifies the compiled ABI version of the validator module. Since the server
 * already enforces the PG_MODULE_MAGIC number for modules across major
 * versions, this is reserved for emergency use within a stable release line.
 * May it never need to change.
 */
#define PG_OAUTH_VALIDATOR_MAGIC 0x20250220

typedef struct OAuthValidatorCallbacks
{
	uint32		magic;			/* must be set to PG_OAUTH_VALIDATOR_MAGIC */

	ValidatorStartupCB startup_cb;
	ValidatorShutdownCB shutdown_cb;
	ValidatorValidateCB validate_cb;
} OAuthValidatorCallbacks;

/*
 * Type of the shared library symbol _PG_oauth_validator_module_init which is
 * required for all validator modules.  This function will be invoked during
 * module loading.
 */
typedef const OAuthValidatorCallbacks *(*OAuthValidatorModuleInit) (void);
extern PGDLLEXPORT const OAuthValidatorCallbacks *_PG_oauth_validator_module_init(void);

/* Implementation */
extern const pg_be_sasl_mech pg_be_oauth_mech;

/*
 * Ensure a validator named in the HBA is permitted by the configuration.
 */
extern bool check_oauth_validator(HbaLine *hba, int elevel, char **err_msg);

#endif							/* PG_OAUTH_H */
