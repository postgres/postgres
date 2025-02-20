# Functions

The `pg_tde` extension provides the following functions:

## pg_tde_add_key_provider_file

Creates a new key provider for the database using a local file.

This function is intended for development, and stores the keys unencrypted in the specified data file.

```
SELECT pg_tde_add_key_provider_file('provider-name','/path/to/the/keyring/data.file');
```

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).

## pg_tde_add_key_provider_vault_v2

Creates a new key provider for the database using a remote HashiCorp Vault server.

The specified access parameters require permission to read and write keys at the location.

```
SELECT pg_tde_add_key_provider_vault_v2('provider-name','secret_token','url','mount','ca_path');
```

where:

* `url` is the URL of the Vault server
* `mount` is the mount point where the keyring should store the keys
* `secret_token` is an access token with read and write access to the above mount point
* [optional] `ca_path` is the path of the CA file used for SSL verification

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).

## pg_tde_add_key_provider_kmip

Creates a new key provider for the database using a remote KMIP server.

The specified access parameters require permission to read and write keys at the server.

```
SELECT pg_tde_add_key_provider_kmip('provider-name','kmip-IP', 5696, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
```

where:

* `provider-name` is the name of the provider. You can specify any name, it's for you to identify the provider.
* `kmip-IP` is the IP address of a domain name of the KMIP server
* The port to communicate with the KMIP server. The default port is `5696`.
* `server-certificate` is the path to the certificate file for the KMIP server.
* `client key` is the path to the client key.

## pg_tde_set_principal_key

Sets the principal key for the database using the specified key provider.

The principal key name is also used for constructing the name in the provider, for example on the remote Vault server.

You can use this function only to a principal key. For changes in the principal key, use the [`pg_tde_rotate_principal_key`](#pg_tde_rotate_principal_key) function.

```
SELECT pg_tde_set_principal_key('name-of-the-principal-key', 'provider-name');
```

## pg_tde_rotate_principal_key

Creates a new version of the specified principal key and updates the database so that it uses the new principal key version.

When used without any parameters, the function will just create a new version of the current database
principal key, using the same provider:

```
SELECT pg_tde_rotate_principal_key();
```

Alternatively, you can pass two parameters to the function, specifying both a new key name and a new provider name:

```
SELECT pg_tde_rotate_principal_key('name-of-the-new-principal-key', 'name-of-the-new-provider');
```

Both parameters support the `NULL` value, which means that the parameter won't be changed:

```
-- creates  new principal key on the same provider as before
SELECT pg_tde_rotate_principal_key('name-of-the-new-principal-key', NULL);

-- copies the current principal key to a new provider
SELECT pg_tde_rotate_principal_key(NULL, 'name-of-the-new-provider');
```


## pg_tde_is_encrypted

Tells if a relation is encrypted using the `pg_tde` extension or not.

To verify that a table is encrypted, run the following statement:

```
SELECT pg_tde_is_encrypted('table_name');
```

You can also verify if the table in a custom schema is encrypted. Pass the schema name for the function as follows:

```
SELECT pg_tde_is_encrypted('schema.table_name');
```

This can additoonally be used the verify that indexes and sequences are encrypted.
