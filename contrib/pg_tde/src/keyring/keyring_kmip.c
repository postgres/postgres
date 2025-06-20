/*
 * KMIP based keyring provider
 */

#include "postgres.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "keyring/keyring_api.h"
#include "keyring/keyring_kmip.h"
#include "keyring/keyring_kmip_impl.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

#define MAX_LOCATE_LEN 128

static void set_key_by_name(GenericKeyring *keyring, KeyInfo *key);
static KeyInfo *get_key_by_name(GenericKeyring *keyring, const char *key_name, KeyringReturnCode *return_code);
static void validate(GenericKeyring *keyring);

const TDEKeyringRoutine keyringKmipRoutine = {
	.keyring_get_key = get_key_by_name,
	.keyring_store_key = set_key_by_name,
	.keyring_validate = validate,
};

void
InstallKmipKeyring(void)
{
	RegisterKeyProviderType(&keyringKmipRoutine, KMIP_KEY_PROVIDER);
}

typedef struct KmipCtx
{
	SSL_CTX    *ssl;
	BIO		   *bio;
} KmipCtx;

static bool
kmipSslConnect(KmipCtx *ctx, KmipKeyring *kmip_keyring, bool throw_error)
{
	SSL		   *ssl = NULL;
	int			level = throw_error ? ERROR : WARNING;

	ctx->ssl = SSL_CTX_new(SSLv23_method());

	if (SSL_CTX_use_certificate_file(ctx->ssl, kmip_keyring->kmip_cert_path, SSL_FILETYPE_PEM) != 1)
	{
		SSL_CTX_free(ctx->ssl);
		ereport(level, errmsg("SSL error: Loading the client certificate failed"));
		return false;
	}

	if (SSL_CTX_use_PrivateKey_file(ctx->ssl, kmip_keyring->kmip_key_path, SSL_FILETYPE_PEM) != 1)
	{
		SSL_CTX_free(ctx->ssl);
		ereport(level, errmsg("SSL error: Loading the client key failed"));
		return false;
	}

	if (SSL_CTX_load_verify_locations(ctx->ssl, kmip_keyring->kmip_ca_path, NULL) != 1)
	{
		SSL_CTX_free(ctx->ssl);
		ereport(level, errmsg("SSL error: Loading the CA certificate failed"));
		return false;
	}

	ctx->bio = BIO_new_ssl_connect(ctx->ssl);
	if (ctx->bio == NULL)
	{
		SSL_CTX_free(ctx->ssl);
		ereport(level, errmsg("SSL error: BIO_new_ssl_connect failed"));
		return false;
	}

	BIO_get_ssl(ctx->bio, &ssl);
	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	BIO_set_conn_hostname(ctx->bio, kmip_keyring->kmip_host);
	BIO_set_conn_port(ctx->bio, kmip_keyring->kmip_port);
	if (BIO_do_connect(ctx->bio) != 1)
	{
		BIO_free_all(ctx->bio);
		SSL_CTX_free(ctx->ssl);
		ereport(level, errmsg("SSL error: BIO_do_connect failed"));
		return false;
	}

	return true;
}

static void
set_key_by_name(GenericKeyring *keyring, KeyInfo *key)
{
	KmipCtx		ctx;
	KmipKeyring *kmip_keyring = (KmipKeyring *) keyring;
	int			result;

	kmipSslConnect(&ctx, kmip_keyring, true);

	result = pg_tde_kmip_set_by_name(ctx.bio, key->name, key->data.data, key->data.len);

	BIO_free_all(ctx.bio);
	SSL_CTX_free(ctx.ssl);

	if (result != 0)
		ereport(ERROR, errmsg("KMIP server reported error on register symmetric key: %i", result));
}

static KeyInfo *
get_key_by_name(GenericKeyring *keyring, const char *key_name, KeyringReturnCode *return_code)
{
	KeyInfo    *key = NULL;
	KmipKeyring *kmip_keyring = (KmipKeyring *) keyring;
	char		id[MAX_LOCATE_LEN];
	KmipCtx		ctx;

	*return_code = KEYRING_CODE_SUCCESS;

	if (!kmipSslConnect(&ctx, kmip_keyring, false))
	{
		return NULL;
	}

	/* 1. locate key */

	{
		int			result;
		size_t		ids_found;

		result = pg_tde_kmip_locate_key(ctx.bio, key_name, &ids_found, id);

		if (result != 0)
		{
			*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		if (ids_found == 0)
		{
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		if (ids_found > 1)
		{
			ereport(WARNING, errmsg("KMIP server contains multiple results for key, ignoring"));
			*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}
	}

	/* 2. get key */

	key = palloc_object(KeyInfo);

	{
		char	   *keyp = NULL;
		int			result = pg_tde_kmip_get_key(ctx.bio, id, &keyp, (int *) &key->data.len);

		if (result != 0)
		{
			ereport(WARNING, errmsg("KMIP server LOCATEd key, but GET failed with %i", result));
			*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
			pfree(key);
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		if (key->data.len > sizeof(key->data.data))
		{
			ereport(WARNING, errmsg("keyring provider returned invalid key size: %d", key->data.len));
			*return_code = KEYRING_CODE_INVALID_KEY;
			pfree(key);
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			free(keyp);
			return NULL;
		}

		memset(key->name, 0, sizeof(key->name));
		memcpy(key->name, key_name, strnlen(key_name, sizeof(key->name) - 1));
		memcpy(key->data.data, keyp, key->data.len);
		free(keyp);
	}

	BIO_free_all(ctx.bio);
	SSL_CTX_free(ctx.ssl);

	return key;
}

static void
validate(GenericKeyring *keyring)
{
	KmipKeyring *kmip_keyring = (KmipKeyring *) keyring;
	KmipCtx		ctx;

	kmipSslConnect(&ctx, kmip_keyring, true);

	BIO_free_all(ctx.bio);
	SSL_CTX_free(ctx.ssl);
}
