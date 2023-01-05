/*
 * Extension to validate the provider token.
 *
 * contrib/oauth_provider/oauth_provider.c
 */

#include "postgres.h"
#include "fmgr.h"
#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

void _PG_init(void);


PG_FUNCTION_INFO_V1(oauth_provider);

/*
 * Returns the current user's authenticated identity.
 */
Datum
oauth_provider(PG_FUNCTION_ARGS)
{
	if (!MyClientConnectionInfo.authn_id)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(MyClientConnectionInfo.authn_id));
}


static int
OAuthTokenProvider(Port *port, const char* token)
{
	/* fill in provider specific token validation logic */
	/* for now, just say token validation is successful */
	if (token == NULL)
		return STATUS_EOF;

	return STATUS_OK;
}

static const char *OAuthError(Port *port)
{
	return psprintf("OAuth bearer authentication failed for user \"%s\"",
					port->user_name);
}

static OAuthProviderOptions *OAuthOptions(Port *port)
{
	StringInfoData	buf;
	OAuthProviderOptions *oauth_end_points;
	initStringInfo(&buf);

	/*
	 * The admin needs to set an issuer and scope for OAuth to work. There's not
	 * really a way to hide this from the user, either, because we can't choose
	 * a "default" issuer, so be honest in the failure message.
	 *
	 * TODO: see if there's a better place to fail, earlier than this.
	 */
	if (!port->hba->oauth_issuer || 
		!port->hba->oauth_scope)
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("OAuth is not properly configured for this user"),
				 errdetail_log("The issuer and scope parameters must be set in pg_hba.conf.")));
	}

	appendStringInfo(&buf, "%s/.well-known/openid-configuration", port->hba->oauth_issuer);
	oauth_end_points = palloc(sizeof(OAuthProviderOptions));
	oauth_end_points->oauth_discovery_uri = buf.data;
	oauth_end_points->scope = port->hba->oauth_scope;
	return oauth_end_points;
}

void
_PG_init(void)
{
	RegisterOAuthProvider(
		"oauth_provider",
		OAuthTokenProvider,
		OAuthError,
		OAuthOptions
	);
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{}