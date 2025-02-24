/*-------------------------------------------------------------------------
 *
 * keyring_kmip.c
 *      KMIP based keyring provider
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/keyring/keyring_kmip.c
 *
 *-------------------------------------------------------------------------
 */

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string.h>
#include <kmip.h>
#include <kmip_bio.h>
#include <kmip_locate.h>

/* The KMIP headers and Postgres headers conflict.
   We can't include most postgres headers here, instead just copy required declarations.
*/

#define bool int
#define true 1
#define false 0

#include "keyring/keyring_kmip.h"
#include "catalog/keyring_min.h"

extern bool RegisterKeyProvider(const TDEKeyringRoutine *routine, ProviderType type);

static KeyringReturnCodes set_key_by_name(GenericKeyring *keyring, keyInfo *key, bool throw_error);
static keyInfo *get_key_by_name(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes *return_code);

const TDEKeyringRoutine keyringKmipRoutine = {
	.keyring_get_key = get_key_by_name,
.keyring_store_key = set_key_by_name};

bool
InstallKmipKeyring(void)
{
	return RegisterKeyProvider(&keyringKmipRoutine, KMIP_KEY_PROVIDER);
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

	ctx->ssl = SSL_CTX_new(SSLv23_method());

	if (SSL_CTX_use_certificate_file(ctx->ssl, kmip_keyring->kmip_cert_path, SSL_FILETYPE_PEM) != 1)
	{
		kmip_ereport(throw_error, "SSL error: Loading the client certificate failed", 0);
		SSL_CTX_free(ctx->ssl);
		return false;
	}

	if (SSL_CTX_use_PrivateKey_file(ctx->ssl, kmip_keyring->kmip_cert_path, SSL_FILETYPE_PEM) != 1)
	{
		SSL_CTX_free(ctx->ssl);
		kmip_ereport(throw_error, "SSL error: Loading the client key failed", 0);
		return false;
	}

	if (SSL_CTX_load_verify_locations(ctx->ssl, kmip_keyring->kmip_ca_path, NULL) != 1)
	{
		SSL_CTX_free(ctx->ssl);
		kmip_ereport(throw_error, "SSL error: Loading the CA certificate failed", 0);
		return false;
	}

	ctx->bio = BIO_new_ssl_connect(ctx->ssl);
	if (ctx->bio == NULL)
	{
		SSL_CTX_free(ctx->ssl);
		kmip_ereport(throw_error, "SSL error: BIO_new_ssl_connect failed", 0);
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
		kmip_ereport(throw_error, "SSL error: BIO_do_connect failed", 0);
		return false;
	}

	return true;
}

static KeyringReturnCodes
set_key_by_name(GenericKeyring *keyring, keyInfo *key, bool throw_error)
{
	KmipCtx		ctx;
	KmipKeyring *kmip_keyring = (KmipKeyring *) keyring;
	int			result;
	int			id_max_len = 64;
	char	   *idp = NULL;

	Attribute	a[4];
	enum cryptographic_algorithm algorithm = KMIP_CRYPTOALG_AES;
	int32		length = key->data.len * 8;
	int32		mask = KMIP_CRYPTOMASK_ENCRYPT | KMIP_CRYPTOMASK_DECRYPT;
	Name		ts;
	TextString	ts2 = {0, 0};
	TemplateAttribute ta = {0};

	if (!kmipSslConnect(&ctx, kmip_keyring, throw_error))
	{
		return KEYRING_CODE_INVALID_RESPONSE;
	}

	for (int i = 0; i < 4; i++)
	{
		kmip_init_attribute(&a[i]);
	}

	a[0].type = KMIP_ATTR_CRYPTOGRAPHIC_ALGORITHM;
	a[0].value = &algorithm;

	a[1].type = KMIP_ATTR_CRYPTOGRAPHIC_LENGTH;
	a[1].value = &length;

	a[2].type = KMIP_ATTR_CRYPTOGRAPHIC_USAGE_MASK;
	a[2].value = &mask;

	ts2.value = key->name.name;
	ts2.size = kmip_strnlen_s(key->name.name, 250);
	ts.value = &ts2;
	ts.type = KMIP_NAME_UNINTERPRETED_TEXT_STRING;
	a[3].type = KMIP_ATTR_NAME;
	a[3].value = &ts;

	ta.attributes = a;
	ta.attribute_count = ARRAY_LENGTH(a);

	result = kmip_bio_register_symmetric_key(ctx.bio, &ta, (char *) key->data.data, key->data.len, &idp, &id_max_len);

	BIO_free_all(ctx.bio);
	SSL_CTX_free(ctx.ssl);

	if (result != 0)
	{
		kmip_ereport(throw_error, "KMIP server reported error on register symmetric key: %i", result);
		return KEYRING_CODE_INVALID_RESPONSE;
	}

	return KEYRING_CODE_SUCCESS;
}

void	   *palloc(size_t);

void		pfree(void *);

static keyInfo *
get_key_by_name(GenericKeyring *keyring, const char *key_name, bool throw_error, KeyringReturnCodes *return_code)
{
	keyInfo    *key = NULL;
	KmipKeyring *kmip_keyring = (KmipKeyring *) keyring;
	char	   *id = 0;
	KmipCtx		ctx;

	*return_code = KEYRING_CODE_SUCCESS;

	if (!kmipSslConnect(&ctx, kmip_keyring, throw_error))
	{
		return NULL;
	}

	/* 1. locate key */

	{
		int			upto = 0;
		int			result;
		LocateResponse locate_result;
		Name		ts;
		TextString	ts2 = {0, 0};
		Attribute	a[3];
		enum object_type loctype = KMIP_OBJTYPE_SYMMETRIC_KEY;

		for (int i = 0; i < 3; i++)
		{
			kmip_init_attribute(&a[i]);
		}

		a[0].type = KMIP_ATTR_OBJECT_TYPE;
		a[0].value = &loctype;

		ts2.value = (char *) key_name;
		ts2.size = kmip_strnlen_s(key_name, 250);
		ts.value = &ts2;
		ts.type = KMIP_NAME_UNINTERPRETED_TEXT_STRING;
		a[1].type = KMIP_ATTR_NAME;
		a[1].value = &ts;

		/* 16 is hard coded: seems like the most vault supports? */
		result = kmip_bio_locate(ctx.bio, a, 2, &locate_result, 16, upto);

		if (result != 0)
		{
			*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		if (locate_result.ids_size == 0)
		{
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		if (locate_result.ids_size > 1)
		{
			fprintf(stderr, "KMIP ERR: %li\n", locate_result.ids_size);
			kmip_ereport(throw_error, "KMIP server contains multiple results for key, ignoring", 0);
			*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		id = locate_result.ids[0];
	}

	/* 2. get key */

	key = palloc(sizeof(keyInfo));

	{
		char	   *keyp = NULL;
		int			result = kmip_bio_get_symmetric_key(ctx.bio, id, strlen(id), &keyp, (int *) &key->data.len);

		if (result != 0)
		{
			kmip_ereport(throw_error, "KMIP server LOCATEd key, but GET failed with %i", result);
			*return_code = KEYRING_CODE_RESOURCE_NOT_AVAILABLE;
			pfree(key);
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			return NULL;
		}

		if (key->data.len > sizeof(key->data.data))
		{
			kmip_ereport(throw_error, "keyring provider returned invalid key size: %d", key->data.len);
			*return_code = KEYRING_CODE_INVALID_KEY_SIZE;
			pfree(key);
			BIO_free_all(ctx.bio);
			SSL_CTX_free(ctx.ssl);
			free(keyp);
			return NULL;
		}

		memcpy(key->data.data, keyp, key->data.len);
		free(keyp);
	}

	BIO_free_all(ctx.bio);
	SSL_CTX_free(ctx.ssl);

	return key;
}
