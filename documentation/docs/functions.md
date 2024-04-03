# Functions

The `pg_tde` extension provides the following functions:

## pg_tde_add_key_provider_file

Creates a new key provider for the database using a local file.

This function is intended for development, and stores the keys unencrypted in the specified data file.

```sql
SELECT pg_tde_add_key_provider_file('provider-name','/path/to/the/keyring/data.file');
```

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).

## pg_tde_add_key_provider_vault_v2

Creates a new key provider for the database using a remote HashiCorp Vault server.

The specified access parameters require permission to read and write keys at the location.

```sql
SELECT pg_tde_add_key_provider_vault_v2('provider-name',:'secret_token','url','mount','ca_path');
```

where:

* `url` is the URL of the Vault server
* `mount` is the mount point where the keyring should store the keys
* `secret_token` is an access token with read and write access to the above mount point
* [optional] `ca_path` is the path of the CA file used for SSL verification

All parameters can be either strings, or JSON objects [referencing remote parameters](external-parameters.md).

## pg_tde_set_master_key

Sets the master key for the database using the specified key provider.

The master key name is also used for constructing the name in the provider, for example on the remote Vault server.

You can use this function only to a master key. For changes in the master key, use the [`pg_tde_rotate_key`](#pg_tde_rotate_key) function.

```sql
SELECT pg_tde_set_master_key('name-of-the-master-key', 'provider-name');
```

## pg_tde_rotate_key

Creates a new version of the specified master key and updates the database so that it uses the new master key version.

When used without any parameters, the function will just create a new version of the current database
master key, using the same provider:

```sql
SELECT pg_tde_rotate_key();
```

Alternatively, you can pass two parameters to the function, specifying both a new key name and a new provider name:

```sql
SELECT pg_tde_rotate_key('name-of-the-new-master-key', 'name-of-the-new-provider');
```

Both parameters support the `NULL` value, which means that the parameter won't be changed:

```sql
-- creates  new master key on the same provider as before
SELECT pg_tde_rotate_key('name-of-the-new-master-key', NULL);

-- copies the current master key to a new provider
SELECT pg_tde_rotate_key(NULL, 'name-of-the-new-provider');
```

## pg_tde_is_encrypted

Tells if a table is using the `pg_tde` access method or not.

```sql
SELECT pg_tde_is_encrypted('table_name');
```


