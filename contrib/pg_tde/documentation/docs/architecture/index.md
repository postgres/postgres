# Architecture

`pg_tde` is a **customizable, complete, data at rest encryption extension** for PostgreSQL.

Let's break down what it means.

**Customizable** means that `pg_tde` aims to support many different use cases:

* Encrypting either every table in every database or only some tables in some databases
* Encryption keys can be stored on various external key storage servers including Hashicorp Vault, KMIP servers.
* Using one key for everything or different keys for different databases
* Storing every key on the same key storage, or using different storages for different databases
* Handling permissions: who can manage database specific or global permissions, who can create encrypted or not encrypted tables

**Complete** means that `pg_tde` aims to encrypt data at rest.

**Data at rest** means everything written to the disk. This includes the following:

* Table data files
* Indexes
* Sequences
* Temporary tables
* Write Ahead Log (WAL)

**Extension** means that `pg_tde` should be implemented only as an extension, possibly compatible with any PostgreSQL distribution, including the open source community version. This requires changes in the PostgreSQL core to make it more extensible. Therefore, `pg_tde` currently works only with the [Percona Server for PostgreSQL](https://docs.percona.com/postgresql/17/index.html) - a binary replacement of community PostgreSQL and included in Percona Distribution for PostgreSQL.

## Main components

The main components of `pg_tde` are the following:

* **Core server changes** focus on making the server more extensible, allowing the main logic of `pg_tde` to remain separate, as an extension. Core changes also add encryption-awareness to some command line tools that have to work directly with encrypted tables or encrypted WAL files.

    [Percona Server for PostgreSQL location](https://github.com/percona/postgres/tree/{{tdebranch}})

* The **`pg_tde` extension itself** implements the encryption code by hooking into the extension points introduced in the core changes, and the already existing extension points in the PostgreSQL server.

    Everything is controllable with GUC variables and SQL statements, similar to other extensions.

* The **keyring API / libraries** implement the key storage logic with different key providers. The API is internal only, the keyring libraries are part of the main library for simplicity.
In the future these could be extracted into separate shared libraries with an open API, allowing the use of third-party providers.

## Encryption architecture

### Two-key hierarchy

`pg_tde` uses two kinds of keys for encryption:

* Internal keys to encrypt the data. They are stored in PostgreSQL's data directory under `$PGDATA/pg_tde`.
* Higher-level keys to encrypt internal keys. These keys are called *principal keys*. They are stored externally, in a Key Management System (KMS) using the key provider API.

`pg_tde` uses one principal key per database. Every internal key for the given database is encrypted using this principal key.

Internal keys are used for specific database files: each file with a different [Object Identifier (OID)](https://www.postgresql.org/docs/current/datatype-oid.html) has a different internal key.

This means that, for example, a table with 4 indexes will have at least 5 internal keys - one for the table, and one for each index.

If a table has additional associated relations, such as sequences or a TOAST table, those relations will also have separate keys.

### Encryption algorithm

`pg_tde` currently uses the following encryption algorithms:

* `AES-128-CBC` for encrypting database files; encrypted with internal keys.

* `AES-128-CTR` for WAL encryption; encrypted with internal keys.

* `AES-128-GCM` for encrypting internal keys; encrypted with the principal key.

Support for other cipher lengths / algorithms is planned in the future.

### Encryption workflow

`pg_tde` makes it possible to encrypt everything or only some tables in some databases.

To support this without metadata changes, encrypted tables are labeled with a `tde_heap` access method marker.

The `tde_heap` access method is the same as the `heap` one. It uses the same functions internally without any changes, but with the different name and ID. In such a way `pg_tde` knows that `tde_heap` tables are encrypted and `heap` tables are not.

The initial decision what to encrypt is made using the `postgres` event trigger mechanism: if a `CREATE TABLE` or `ALTER TABLE` statement uses the `tde_heap` clause, the newly created data files are marked as encrypted. Then file operations encrypt or decrypt the data.

Later decisions are made using a slightly modified Storage Manager (SMGR) API: when a database file is re-created with a different ID as a result of a `TRUNCATE` or a `VACUUM FULL` command, the newly created file inherits the encryption information and is either encrypted or not.

### WAL encryption

WAL encryption is controlled globally via a global GUC variable, `pg_tde.wal_encrypt`, that requires a server restart.

WAL keys also contain the [LSN](https://www.postgresql.org/docs/17/wal-internals.html) of the first WAL write after key creation. This allows `pg_tde` to know which WAL ranges are encrypted or not and with which key.

The setting only controls writes so that only WAL writes are encrypted when WAL encryption is enabled. This means that WAL files can contain both encrypted and unencrpyted data, depending on what the status of this variable was when writing the data.

`pg_tde` keeps track of the encryption status of WAL records using internal keys. When the server is restarted it writes a new internal key if WAL encryption is enabled, or if it is disabled and was previously enabled it writes a dummy key signalling that WAL encryption ended.

With this information the WAL reader code can decide if a specific WAL record has to be decrypted or not and which key it should use to decrypt it.

### Encrypting other access methods

Currently `pg_tde` only encrypts `heap` tables and other files such as indexes, TOAST tables, sequences that are related to the `heap` tables.

Indexes include any kind of index that goes through the SMGR API, not just the built-in indexes in PostgreSQL.

In theory, it is also possible to encrypt any other table access method that goes through the SMGR API by similarly providing a marker access method to it and extending the event triggers.

### Storage Manager (SMGR) API

`pg_tde` relies on a slightly modified version of the SMGR API. These modifications include:

* Making the API generally extensible, where extensions can inject custom code into the storage manager
* Adding tracking information for files. When a new file is created for an existing relation, references to the existing file are also passed to the SMGR functions

With these modifications, the `pg_tde` extension can implement an additional layer on top of the normal Magnetic Disk SMGR API: if the related table is encrypted, `pg_tde` encrypts a file before writing it to the disk and, similarly, decrypts it after reading when needed.

## Key and key provider management

### Principal key rotation

You can rotate principal keys to comply with common policies and to handle situations with potentially exposed principal keys.

Rotation means that `pg_tde` generates a new version of the principal key, and re-encrypts the associated internal keys with the new key. The old principal key is kept as is at the same location, because it may still be needed to decrypt backups or other databases.

### Internal key regeneration

Internal keys for tables, indexes and other data files are fixed once a file is created. There's no way to re-encrypt a file.

There are workarounds for this, because operations that move the table data to a new file, such as `VACUUM FULL` or an `ALTER TABLE` that rewrites the file will create a new key for the new file, essentially rotating the internal key. This however means taking an exclusive lock on the table for the duration of the operation, which might not be desirable for huge tables.

WAL internal keys are also fixed to the respective ranges. To generate a new WAL key you need to restart the database.

### Internal key storage

Internal keys and `pg_tde` metadata in general are kept in a single `$PGDATA/pg_tde` directory. This directory stores separate files for each database, such as:

* Encrypted internal keys and internal key mapping to tables
* Information about the key providers

Also, the `$PGDATA/pg_tde` directory has a special global section marked with the OID `1664`, which includes the global key providers and global internal keys.

The global section is used for WAL encryption. Specific databases can use the global section too, for scenarios where users configure individual principal keys for databases but use the same global key provider. For this purpose, you must enable the global provider inheritance.

The global default principal key uses the special OID `1663`.

### Key providers (principal key storage)

Principal keys are stored externally in a Key Management Services (KMS). In `pg_tde`a KMS is defined as an external key provider.

The following key providers are supported:

* [HashiCorp Vault](https://developer.hashicorp.com/vault/docs/what-is-vault) KV2 secrets engine
* [OpenBao](https://openbao.org/) implementation of Vault
* KMIP compatible servers
* A local file storage. This storage is intended only for development and testing and is not recommended for production use.

For each key provider `pg_tde` requires a detailed configuration including the address of the service and the authentication information.

With these details `pg_tde` does the following based on user operations:

* Uploads a new principal key to it after this key is created
* Retrieves the principal key from the service when it is required for decryption

Retreival of the principal key is cached so it only happens when necessary.

### Key provider management

Key provider configuration or location may change. For example, a service is moved to a new address or the principal key must be moved to a different key provider type. `pg_tde` supports both these scenarios enabling you to manage principal keys using simple [SQL functions](../functions.md#key-provider-management).

In certain cases you can't use SQL functions to manage key providers. For example, if the key provider changed while the server wasn't running and is therefore unaware of these changes. The startup can fail if it needs to access the encryption keys.

For such situations, `pg_tde` also provides [command line tools](../command-line-tools/index.md) to recover the database.

### Sensitive key provider information

!!! important

    Authentication details for key providers are sensitive and must be protected.
    Do not store these credentials in the `$PGDATA` directory alongside the database. Instead, ensure they are stored in a secure location with strict file system permissions to prevent unauthorized access.

## User interface

### Setting up pg_tde

To use `pg_tde`, users are required to:

* Add `pg_tde` to the `shared_preload_libraries` in `postgresql.conf` as this is required for the SMGR extensions
* Execute `CREATE EXTENSION pg_tde` in the databases where they want to use encryption
* Optionally, enable `pg_tde.wal_encrypt` in `postgresql.conf`
* Optionally, disable `pg_tde.inherit_global_providers` in `postgresql.conf` (enabled by default)

### Adding providers

Keyring providers can be added to either the global or to the database specific scope.

If `pg_tde.inherit_global_providers` is `on`, global providers are visible for all databases, and can be used.
If `pg_tde.inherit_global_providers` is `off`, global providers are only used for WAL encryption.

To add a global provider:

```sql
pg_tde_add_global_key_provider_<TYPE>('provider_name', ... details ...)
```

To add a database specific provider:

```sql
pg_tde_add_database_key_provider_<TYPE>('provider_name', ... details ...)
```

### Changing providers

To change a value of a global provider:

```sql
pg_tde_change_global_key_provider_<TYPE>('provider_name', ... details ...)
```

To change a value of a database specific provider:

```sql
pg_tde_change_database_key_provider_<TYPE>('provider_name', ... details ...)
```

These functions also allow changing the type of a provider.

The functions however do not migrate any data. They are expected to be used during infrastructure migration, for example when the address of a server changes.

Note that in these functions do not verify the parameters. For that, see `pg_tde_verify_key`.

### Changing providers from the command line

To change a provider from a command line, `pg_tde` provides the `pg_tde_change_key_provider` command line tool.

This tool work similarly to the above functions, with the following syntax:

```bash
pg_tde_change_key_provider <dbOid> <providerType> ... details ...
```

Note that since this tool is expected to be offline, it bypasses all permission checks!

This is also the reason why it requires a `dbOid` instead of a name, as it has no way to process the catalog and look up names.

### Deleting providers

Providers can be deleted by using the

```sql
pg_tde_delete_database_key_provider(provider_name)
pg_tde_delete_global_key_provider(provider_name)
```

functions.

For database specific providers, the function first checks if the provider is used or not, and the provider is only deleted if it's not used.

For global providers, the function checks if the provider is used anywhere, WAL or any specific database, and returns an error if it is.

This somewhat goes against the principle that `pg_tde` should not interact with other databases than the one the user is connected to, but on the other hand, it only does this lookup in the internal `pg_tde` metadata, not in postgres catalogs, so it is a gray zone. Making this check makes more sense than potentially making some databases inaccessible.

### Listing/querying providers

`pg_tde` provides 2 functions to show providers:

* `pg_tde_list_all_database_key_providers()`
* `pg_tde_list_all_global_key_providers()`

These functions return a list of provider names, type and configuration.

### Provider permissions

`pg_tde` implements access control based on execution rights on the administration functions.

For keys and providers administration, it provides two pair of functions:

```sql
pg_tde_GRANT_database_key_management_TO_role
pg_tde_REVOKE_database_key_management_FROM_role
```

### Creating and rotating keys

Principal keys can be created or rotated using the following functions:

```sql
pg_tde_set_key_using_(global/database)_key_provider('key-name', 'provider-name', ensure_new_key)
pg_tde_set_server_key_using_(global/database)_key_provider('key-name', 'provider-name', ensure_new_key)
pg_tde_set_default_key_using_(global/database)_key_provider('key-name', 'provider-name', ensure_new_key)
```

`ensure_new_key` is a boolean parameter defaulting to false. If it is `true` the function might return an error instead of setting the key if it already exists on the provider.

### Default principal key

With `pg_tde.inherit_global_key_providers`, it is also possible to set up a default global principal key, which will be used by any database which has the `pg_tde` extension enabled, but doesn't have a database specific principal key configured using `pg_tde_set_key_using_(global/database)_key_provider`.

With this feature, it is possible for the entire database server to easily use the same principal key for all databases, completely disabling multi-tenency.

#### Manage a default key

You can manage a default key with the following functions:

* `pg_tde_set_default_key_using_global_key_provider('key-name','provider-name','true/false')`
* `pg_tde_delete_default_key()`

!!! note
    `pg_tde_delete_default_key()` is only possible if there's no database currently using the default principal key.
    Changing the default principal key will rotate the encryption of internal keys for all databases using the current default principal key.

#### Delete a key

The `pg_tde_delete_key()` function removes the principal key for the current database. If the current database has any encrypted tables, and there isn’t a default principal key configured, it reports an error instead. If there are encrypted tables, but there’s also a default principal key, internal keys will be encrypted with the default key.

!!! note
    WAL keys **cannot** be deleted, as server keys are managed separately.

### Current key details

`pg_tde_key_info()` returns the name of the current principal key, and the provider it uses.

`pg_tde_server_key_info()` does the same for the server key.

`pg_tde_default_key_info()` does the same for the default key.

`pg_tde_verify_key()` checks that the key provider is accessible, that the current principal key can be downloaded from it, and that it is the same as the current key stored in memory - if any of these fail, it reports an appropriate error.

### Key permissions

Users with management permissions to a specific database `(pg_tde_(grant/revoke)_(global/databse)_key_management_(to/from)_role)` can change the keys for the database, and use the current key functions. This includes creating keys using global providers, if `pg_tde.inherit_global_providers` is enabled.

Also the `pg_tde_(grant/revoke)_database_key_management_to_role` function deals with only the specific permission for the above function: it allows a user to change the key for the database, but not to modify the provider configuration.

### Creating encrypted tables

To create an encrypted table or modify an existing table to be encrypted, use the following commands:

```sql
CREATE TABLE t1(a INT) USING tde_heap;
ALTER TABLE t1 SET ACCESS METHOD tde_heap;
```

### Changing the `pg_tde.inherit_global_keys` setting

It is possible for users to use `pg_tde` with `inherit_global_keys = on`, refer to global keys / keyrings in databases, and then change this setting to `off`.

In this case existing references to global providers, or the global default principal key will remain working as before, but new references to the global scope can't be made.

## Typical setup scenarios

### Simple "one principal key" encryption

1. Passing the option from the postgres config file the extension: `shared_preload_libraries=‘pg_tde’`
2. `CREATE EXTENSION pg_tde;` in `template1`
3. Adding a global key provider
4. Adding a default principal key using the same global provider
5. Enable WAL encryption to use the default principal key using `ALTER SYSTEM SET pg_tde.wal_encrypt=‘ON’`
6. Restart the server
7. Optionally: setting the `default_table_access_method` to `tde_heap` so that tables are encrypted by default

Database users don't need permissions to any of the encryption functions:
encryption is managed by the admins, normal users only have to create tables with encryption, which requires no specific permissions.

### One key storage, but different keys per database

1. Installing the extension: `shared_preload_libraries` + `pg_tde.wal_encrypt`
2. `CREATE EXTENSION pg_tde;` in `template1`
3. Adding a global key provider
4. Changing the WAL encryption to use the proper global key provider
5. Giving users that are expected to manage database keys permissions for database specific key management, but not database specific key provider management:
   specific databases HAVE to use the global key provider

Note: setting the `default_table_access_method` to `tde_heap` is possible, but instead of `ALTER SYSTEM` only per database using `ALTER DATABASE`, after a principal key is configured for that specific database.

Alternatively `ALTER SYSTEM` is possible, but table creation in the database will fail if there's no principal key for the database, that has to be created first.

### Complete multi tenancy

1. Installing the extension: `shared_preload_libraries` + `pg_tde.wal_encrypt` (that's not multi tenant currently)
2. `CREATE EXTENSION pg_tde;` in any database
3. Adding a global key provider for WAL
4. Changing the WAL encryption to use the proper global key provider

No default configuration: key providers / principal keys are configured as a per database level, permissions are managed per database

Same note about `default_table_access_method` as above - but in a multi tenant setup, `ALTER SYSTEM` doesn't make much sense.
