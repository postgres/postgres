# Functions

The `pg_tde` extension provides the following functions:

## Permission management

By default, `pg_tde` is restrictive:
management functions are only available to superusers.

Permissions are based on the normal `EXECUTE` permission on the functions provided by `pg_tde`, and can be managed by `GRANT EXECUTE` and `REVOKE EXECUTE`.

The following functions are also provided for easier management of functionality groups:

### Database local key management

These functions allow to grant or revoke permissions to manage permissions of the current database, they enable or disable all functions that deal with providers and keys on the current database:

* `pg_tde_grant_local_key_management_to_role(role)`
* `pg_tde_revoke_local_key_management_from_role(role)`

### Global scope ky management

These functions allow to grant or revoke permissions to manage permissions of the global scope, they enable or disable all functions that deal with providers and keys for the global scope:

* `pg_tde_grant_global_key_management_to_role(role)`
* `pg_tde_revoke_global_key_management_from_role(role)`

### Permission management

These functions allow or revoke the use of the permissions management functions:

* `pg_tde_grant_grant_management_to_role(role)`
* `pg_tde_revoke_grant_management_from_role(role)`


### Inspections

These functions allow or revoke the use of query functions, that do not modify the encryption settings:

* `pg_tde_grant_key_viewer_management_to_role(role)`
* `pg_tde_revoke_key_viewer_management_from_role(role)`


## Key provider management

`pg_tde` implements support for multiple key providers.

Currently the list of supported providers:

* local file
* Hashicorp Vault / OpenBao
* KMIP compatible providers

Key providers have four operations: creating a new provider, changing an existing provider, deleting a provider, and listing key providers.

Creation and modification is implemented by provider specific functions, implemted by all provider types:

* `pg_tde_add_key_provider_<type>('provider-name', <provider specific parameters>)`
* `pg_tde_add_global_key_provider_<type>('provider-name', <provider specific parameters>)`
* `pg_tde_change_key_provider_<type>('provider-name', <provider specific parameters>)`
* `pg_tde_change_global_key_provider_<type>('provider-name', <provider specific parameters>)`

There are two functions for adding providers for each implementation:
one to add a provider to the current database, and another to add a provider to the global scope.

Similarly, there are two functions to change existing providers: 
one to change a provider in the current database, and one to change a provider in the global scope.

When adding a new provider, the name has to be unqiue in the scope, but a database local and a global provider can have the same name.

When changing a provider, the referred name has to exist in the database local or global scope.
The `change` functions require the same parameters as the `add` functions, and overwrite the setting for every parameter except for the name, which can't be changed.

Provider specific parameters are different for each implemntation, and discussed in the matching subsections.

**Some provider specific parameters contain sensitive information, such as passwords. Never specify these directly, use the remote configuration option instead.**

#### Deleting providers

These functions delete an existing provider in the corrent database or in the global scope:

* `pg_tde_delete_key_provider('provider-name)`
* `pg_tde_delete_global_key_provider('provider-name)`

Key providers can only be deleted when not currently in use.
If the current principal key is using the specific provider, an error is returned instead.

If `pg_tde.inherit_global` is enabled, a global key provider can only be deleted if it isn't used anywhere, including any databases.
If it is used in any databases, an error is returned instead.

#### Listing key providers

These functions list the details of all key providers for the current database or for the global scope, including all configuration values:

* `pg_tde_list_all_key_providers()`
* `pg_tde_list_all_global_key_providers()`

**All configuration values include possibly sensitive values, such as passwords. Never specify these directly, use the remote configuration option instead.**


#### Adding or modifying Vault providers

The Vault provider connect to a HashiCorp Vault or OpenBao server, and store the keys on a key-value store version 2.

It provides the following functions

```
SELECT pg_tde_add_key_provider_vault_v2('provider-name','secret_token','url','mount','ca_path');
SELECT pg_tde_add_global_key_provider_vault_v2('provider-name','secret_token','url','mount','ca_path');
SELECT pg_tde_change_key_provider_vault_v2('provider-name','secret_token','url','mount','ca_path');
SELECT pg_tde_change_global_key_provider_vault_v2('provider-name','secret_token','url','mount','ca_path');
```

where:

* `provider-name` is the name of the key provider
* `url` is the URL of the Vault server
* `mount` is the mount point on the Vault server where the keyring should store the keys
* `secret_token` is an access token with read and write access to the above mount point
* [optional] `ca_path` is the path of the CA file used for SSL verification

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).

**Never specify the secret token directly, use a remote parameter instead.**


### Adding or modifying KMIP providers

The KMIP provider uses a remote KMIP server.

```
SELECT pg_tde_add_key_provider_kmip('provider-name','kmip-addr', `port`, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
SELECT pg_tde_add_global_key_provider_kmip('provider-name','kmip-addr', `port`, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
SELECT pg_tde_change_key_provider_kmip('provider-name','kmip-addr', `port`, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
SELECT pg_tde_change_global_key_provider_kmip('provider-name','kmip-addr', `port`, '/path_to/server_certificate.pem', '/path_to/client_key.pem');
```

where:

* `provider-name` is the name of the provider
* `kmip-addr` is the IP address or domain name of the KMIP server
* `port` is the port to communicate with the KMIP server.
  Most KMIP servers use port 5696.
* `server-certificate` is the path to the certificate file for the KMIP server.
* `client key` is the path to the client key.

The specified access parameters require permission to read and write keys at the server.

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).

### Adding or modifying Local file providers

The local file provider manages a key database using a local file.

This function is intended for development or quick testing, and stores the keys unencrypted in the specified data file.

**It is not recommended for production.**

```
SELECT pg_tde_add_key_provider_file('provider-name','/path/to/the/keyring/data.file');
SELECT pg_tde_add_global_key_provider_file('provider-name','/path/to/the/keyring/data.file');
SELECT pg_tde_change_key_provider_file('provider-name','/path/to/the/keyring/data.file');
SELECT pg_tde_change_global_key_provider_file('provider-name','/path/to/the/keyring/data.file');
```

where:

* `provider-name` is the name of the provider. You can specify any name, it's for you to identify the provider.
* `/path/to/the/keyring/data.file` is the path to the keyring file.

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).



## Principal key management

These functions can be used to create a new principal key for a speciifc scope (current database, global, default), or to start using a different existing key for a specific scope.

Princial keys are stored on key providers by the name specified in this function - for example, when using the Vault provider, after creating a key named "foo", a key named "foo" will be visible on the Vault server at the specified mount point.

### pg_tde_set_principal_key

Creates or rotates the principal key for the database using the specified key provider and name.

```
SELECT pg_tde_set_principal_key('name-of-the-principal-key','provider-name','ensure_new_key');
```

 The `ensure_new_key` parameter instructs the function how to handle a principal key during key rotation:

* If set to `true` (default), a new key must be unique.
  If the provider already stores a key by that name, the function returns an error.
* If set to `false`, an existing principal key will be reused.

### pg_tde_set_server_principal_key

Creates or rotates the global principal key for the database using the specified key provider. Use this function to set a principal key for WAL encryption.

```
SELECT pg_tde_set_principal_key('name-of-the-principal-key','provider-name','ensure_new_key');
```

The `ensure_new_key` parameter instructs the function how to handle a principal key during key rotation:

* If set to `true` (default), a new key must be unique. 
  If the provider already stores a key by that name, the function returns an error.
* If set to `false`, an existing principal key will be reused.


### pg_tde_set_default_principal_key

Creates or rotates the default principal key for the server using the specified key provider.

The default key is automatically used by any database that doesn't have a specific key created the first time an encrypted database object is created.

```
SELECT pg_tde_set_default_principal_key('name-of-the-principal-key','provider-name','ensure_new_key');
```

The `ensure_new_key` parameter instructs the function how to handle a principal key during key rotation:

* If set to `true` (default), a new key must be unique. 
  If the provider already stores a key by that name, the function returns an error.
* If set to `false`, an existing principal key will be reused.

## Encryption status check

### pg_tde_is_encrypted

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

### pg_tde_principal_key_info

Displays information about the principal key for the current database, if exists.

```
SELECT pg_tde_principal_key_info()
```

### pg_tde_global_principal_key_info

Displays information about the principal key for the global scope, if exists.

```
SELECT pg_tde_global_principal_key_info()
```

### pg_tde_verify_principal_key

This function checks that the current database has a properly functional encryption setup, which means:

* A keyring is configured
* The keyring is accessible using the specified configuration
* There is a principal key for the database
* The principal key can be retrieved from the remote keyring
* The principal key returned from the keyring is the same as cached in the server memory

If any of the above checks fail, the function will report an error.

```
SELECT pg_tde_verify_principal_key()
```

### pg_tde_verify_global_principal_key

This function checks that the global scope has a properly functional encryption setup, which means:

* A keyring is configured
* The keyring is accessible using the specified configuration
* There is a principal key for the global scope
* The principal key can be retrieved from the remote keyring
* The principal key returned from the keyring is the same as cached in the server memory

If any of the above checks fail, the function will report an error.

```
SELECT pg_tde_verify_principal_key()
```
