# PG_TDE High Level Overview


## Goals

Pg_tde aims to be a (1) customizable, (2) complete, (3) data at rest encryption (4) extension for PostgreSQL.

Customizable (1) means that `pg_tde` aims to support many different use cases:

* Encrypting every table in every database or only some tables in some databases
* Encryption keys can be stored on various external key storage servers (Hashicorp Vault, KMIP servers, ...)
* Using one key for everything or different keys for different databases
* Storing every key at the same key storage, or using different storages for different databases
* Handling permissions: who can manage database specific or global permissions, who can create encrypted or not encrypted tables

Complete (2) means that `pg_tde` aims to encrypt everything written to the disk: data at rest (3).
This includes:

* Table data files
* Indexes
* Temporary tables
* Write ahead log
* PG-994 System tables (not yet implemented)
* PG-993 Temporary files (not yet implemented)

Extension (4) means that ideally `pg_tde` entirely should be implemented only as an extension, possibly compatible with any PostgreSQL distribution, including the open source community version.
This requires changes in the core (making it more extensible), which means currently `pg_tde` only works with the Percona Distribution.

## Main components

Pg_tde consist of 3 main components:

Core server changes focus on making the server more extensible, allowing the main logic of pg_tde to remain separate as an extension.
Additionally, core changes also add encryption-awareness to some command line tools that have to work directly with encrypted tables or encrypted WAL.
Alternatively, these could be implemented as duplicated tools in part of the extension, but keeping everything in the original tool allows a better user experience, as currently `pg_tde` requires the Percona Distribution anyway.

Our patched postgres can be found at the following location:
https://github.com/percona/postgres/tree/TDE_REL_17_STABLE 

The `pg_tde` extension implements the encryption code itself by hooking into the extension points introduced in the core changes, and the already existing extension points in the PostgreSQL server.
Everything is controllable with GUC variables and SQL statements, similar to other extensions.

Finally, the keyring API / libraries implement actual key storage logic with different providers.
In the future these could be extracted into separate shared libraries with an open API, allowing third party providers.
Currently the API already exists, but it is internal only and keyring libraries are part of the main library for simplicity.

## Encryption architecture

### Two key hierarchy

`Pg_tde` uses a two level key structure, also found commonly in other database servers:

The higher level keys are called "principal keys". These are the keys that are stored externally using the keyring APIs, and these are used to encrypt the "internal keys".

`Pg_tde` uses one principal key per database, every internal key for the given database is encrypted using this principal key.

Internal keys are used for specific database files: each file that has a different Oid and has a different internal key.
This means that for example a table with 4 indexes will have at least 5 internal keys - one for the table, and one for each index.
If the table has additional files, such as sequence(s) or a toast table, those files also have separate keys.

### Encryption algorithm

`Pg_tde` currently uses a hardcoded AES-CBC-128 algorithm for encrypting most database files.
First the internal keys in the datafile are encrypted using the principal key with AES-CBC-128, then the file data itself is again encrypted using AES-CBC-128 with the internal key.

WAL encryption is different, it uses AES-CTR-128.

Support for other cipher lengths / algorithms is planned in the future (PG-1194).

### Marking encrypted objects

`Pg_tde` makes it possible to encrypt everything, or only some tables in some databases.
To support this without metadata changes, encrypted tables are marked with a marker access method:
`tde_heap` is the same as `heap`, it uses the same functions internally without any changes, but with the different name and Id, `pg_tde` knows that `tde_heap` tables are encrypted, and `heap` tables aren't.

The initial decision is made using the postgres event trigger mechanism:
if a `CREATE TABLE` or `ALTER TABLE` statement uses `tde_heap`, the newly created data files are marked as encrypted, and then file operations encrypt/decrypt the data.

Later decisions are made using a slightly modified SMGR API:
when a database file is recreated with a different Id, for example because of a `TRUNCATE` or `VACUUM FULL`, the new file is either encrypted or plain based on the encryption information of the previous file.

### SMGR API

`Pg_tde` relies on a slightly modified version of the SMGR API.
These modifications include:

* Making the API generally extensible, where extensions can inject custom code into the storage manager
* Adding tracking information for files:
  when a new file is created for an existing relation, references to the existing file are also passed to the smgr functions

With these modifications, the `pg_tde` extension can implement an additional layer on top of the normal MD SMGR API, which encrypts a file before writing if the related table is encrypted, and similarly decrypts it after reading when needed.

### Encrypting other access methods

Currently `pg_tde` only encrypts heap tables and other files (indexes, toast tables, sequences) related to the heap tables.
Indexes include any kind of index that goes through the SMGR API, not just the built-in indexes in postgres.

In theory, it is possible to also encrypt any other table access method that goes through the SMGR API, by similarly providing a marker access method to it and extending the event triggers.

### WAL encryption

WAL encryption is currently controlled globally, it's either turned on or off with a global GUC variable that requires a server restart.
The variable only controls writes, when it is turned on, WAL writes are encrypted.

This means WAL files can contain both encrypted and unencrpyted data, depending on what the status of this variable was when writing the data.
`pg_tde` keeps track of the encryption status of WAL records using internal keys:
every time the encryption status of WAL changes, it writes a new internal key for WAL.
When the encryption is turned on, this internal key contains a valid encryption key.
When the encrpytion is turned off, it only contains a flag signaling that WAL encryption ended.

With this information, the WAL reader code can decide if a specific WAL records has to be decrypted or not.

### Principal key rotation

To comply with common policies, and to handle situations with potentially exposed principal keys, principal keys can be rotated.
Rotation means that `pg_tde` generates a new version of the principal key, and re-encrypts the associated internal keys with the new key.
The old key is kept as is at the same location, as it is potentially still needed to decrypt backups or other databases.

It is also possible that the location of the keyring changes:
either the service is moved to a new address, or the keyring has to be moved to a different keyring provider type. Both of these situations are supported by `pg_tde` using simple SQL functions.

In certain cases the SQL API can't be used for these operations:
if the server isn't running, and the old keyring is no longer available, startup can fail if it needs to access the encryption keys. 
For these situations, `pg_tde` also provides command line tools to recover the database.


### Internal key regeneration

Internal keys for files (tables, indexes, etc.) are fixed once the file is created, there's no way currently to re-encrypt a file.

There are workarounds for this, because operations that move the table data to a new file, such as `VACUUM FULL` or an `ALTER TABLE` that rewrites the file will create a new key for the new file, essentially rotating the internal key.
This however means taking an exclusive lock on the table for the duration of the operation, which might not be desirable for huge tables.

### Internal key storage

Internal keys, and generally `pg_tde` metadata is kept in a single directory in `$PGDATA/pg_tde`. 
In this directory each database has separate files, containing:

* Encrypted internal keys and internal key mapping to tables
* Information about the key providers

There's also a special global section marked with the OID 607, which includes the global key providers / global internal keys.

This is used by the WAL encryption, and can optionally be used by specific databases too, if global provider inheritance is enabled.

### Key providers (principal key storage)

Principal keys are stored on external providers.
Currently pg_tde has 3 implementations:

* A local file storage, intended for development and testing only
* Hashicorp Vault
* KMIP compatible servers

In all cases, `pg_tde` requires a detailed configuration, including the address of the service and authentication information.
With these details, all operations are done by `pg_tde` based on user operations:

* When a new principal key is created, it will communicate with the service and upload a fresh key to it
* When a principal key is required for decryption, it will try to get the key from the service

### Sensitive key provider information

Some of the keyring provider information (authentication details) is potentially sensitive, and it is not safe to store it together with the database in the `$PGDATA` directory, or even on the same server.

For this purpose, `pg_tde` provides a mechanism where instead of directly specifying the parameter, users can specify an external service instead, from where it downloads the information.
This way the configuration itself only contains the reference, not the actual authentication key or password.

Currently only HTTP or external file references are supported, but other possible mechanisms can be added later, such as kubernetes secrets.

## User interface

### Setting up pg_tde

To use `pg_tde`, users are required to:

* Add pg_tde to the `shared_preload_libraries` in `postgresql.conf`, as this is required for the SMGR extensions
* Execute `CREATE EXTENSION pg_tde` in the databases where they want to use encryption
* Optionally, enable `pg_tde.wal_encrypt` in `postgresql.conf`
* Optionally, disable `pg_tde.inherit_global_providers` in `postgresql.conf` (enabled by default)

### Adding providers

Keyring providers can be added to either the GLOBAL or to the database specific scope.

If `pg_tde.inherit_global_providers` is `ON`, global providers are visible for all databases, and can be used.
If `pg_tde.inherit_global_providers` is `OFF`, global providers are only used for WAL encryption.

To add a global provider:

```sql
pg_tde_add_global_key_provider_<TYPE>(‘provider_name', ... details ...)
```

To add a database specific provider:

```sql
pg_tde_add_key_provider_<TYPE>(‘provider_name', ... details ...)
```

Note that in these functions do not verify the parameters.
For that, see `pg_tde_verify_principal_key`.

### Changing providers

To change a value of a global provider:

```sql
pg_tde_modify_global_key_provider_<TYPE>(‘provider_name', ... details ...)
```

To change a value of a database specific provider:

```sql
pg_tde_modify_key_provider_<TYPE>(‘provider_name', ... details ...)
```

These functions also allow changing the type of a provider.

The functions however do not migrate any data.
They are expected to be used during infrastructure migration, for example when the address of a server changes.

Note that in these functions do not verify the parameters.
For that, see `pg_tde_verify_principal_key`.

### Changing providers from the command line

To change a provider from a command line, `pg_tde` provides the `pg_tde_modify_key_provider` command line tool.

This tool work similarly to the above functions, with the following syntax:

```bash
pg_tde_modify_key_provider <dbOid> <providerType> ... details ...
```

Note that since this tool is expected to be offline, it bypasses all permission checks!

This is also the reason why it requires a `dbOid` instead of a name, as it has no way to process the catalog and look up names.

### Deleting providers

Providers can be deleted by the 

```sql
pg_tde_delete_key_provider(provider_name) 
pg_tde_delete_global_key_provider(provider_name) 
```

functions.

For database specific providers, the function first checks if the provider is used or not, and the provider is only deleted if it's not used.

For global providers, the function checks if the provider is used anywhere, WAL or any specific database, and returns an error if it is.

This somewhat goes against the principle that `pg_tde` shouldn't interact with other databases than the one the user is connected to, but on the other hand, it only does this lookup in the internal `pg_tde` metadata, not in postgres catalogs, so it is a gray zone.
Making this check makes more sense than potentially making some databases inaccessible.

### Listing/querying providers

`Pg_tde` provides 2 functions to show providers:

* `pg_tde_list_all_key_providers()`
* `pg_tde_list_all_global_key_providers()`

These functions only return a list of provider names, without any details about the type/configuration.

PG-??? There's also two function to query the details of providers:

```sql
pg_tde_show_key_provider_configuration(‘provider-name')
pg_tde_show_global_key_provider_configuration(‘provider-name')
```

These functions display the provider type and configuration details, but won't show the sensitive parameters, such as passwords or authentication keys.

### Provider permissions

`Pg_tde` implements access control based on execution rights on the administration functions.

For provider administration, it provides two pair of functions:

```sql
pg_tde_(grant/revoke)_local_provider_management_to_role
pg_tde_(grant/revoke)_global_provider_management_to_role
```

There's one special behavior:
When `pg_tde.inherit_global_providers` is ON, users with database local permissions can list global providers, but can't use the show function to query configuration details.
When `pg_tde.inherit_global_providers` is OFF, they can't execute the function at all, it will return an error.

### Creating and rotating keys

Principal keys can be created or rotated using the following functions:

```sql
pg_tde_set_principal_key(‘key-name', ‘provider-name', ensure_new_key)
pg_tde_set_global_principal_key(‘key-name', ‘provider-name', ensure_new_key)
pg_tde_set_server_principal_key(‘key-name', ‘provider-name', ensure_new_key)
```

`Ensure_new_key` is a boolean parameter defaulting to false.
If it is true, the function might return an error instead of setting the key, if it already exists on the provider.

### Default principal key

With `pg_tde.inherit_global_key_providers`, it is also possible to set up a default global principal key, which will be used by any database which has the `pg_tde` extension enabled, but doesn't have a database specific principal key configured using `pg_tde_set_(global_)principal_key`.

With this feature, it is possible for the entire database server to easily use the same principal key for all databases, completely disabling multi-tenency.

A default key can be managed with the following functions:

```sql
pg_tde_set_default_principal_key(‘key-name', ‘provider-name', ensure_new_key)
pg_tde_drop_default_principal_key() -- not yet implemented
```

`DROP` is only possible if there's no table currently using the default principal key.

Changing the default principal key will rotate the encryption of internal keys for all databases using the current default principal key.

### Removing key (not yet implemented)

`pg_tde_drop_principal_key` removes the principal key for the current database.
If the current database has any encrypted tables, and there isn't a default principal key configured, it reports an error instead.
If there are encrypted tables, but there's also a global default principal key, internal keys will be encrypted with the default key.

It isn't possible to remove the WAL (server) principal key.

### Current key details

`pg_tde_principal_key_info()` returns the name of the current principal key, and the provider it uses.

`pg_tde_global_principal_key_info(‘PG_TDE_GLOBAL')` does the same for the server key.

`pg_tde_verify_principal_key()` checks that the key provider is accessible, that the current principal key can be downloaded from it, and that it is the same as the current key stored in memory - if any of these fail, it reports an appropriate error.

### Listing all active keys (not yet implemented)

SUPERusers are able to use the following function:

`pg_tde_list_active_keys()`

Which reports all the actively used keys by all databases on the current server.
Similarly to `pg_tde_show_current_principal_key`, it only shows names and associated providers, it doesn't reveal any sensitive information about the providers.

### Key permissions

Users with management permissions to a specific database `(pg_tde_(grant/revoke)_provider_management_to_role)` can change the keys for the database, and use the current key functions. This includes creating keys using global providers, if `pg_tde.inherit_global_providers` is enabled.

Also, the `pg_tde_(grant/revoke)_key_management_to_role` function deals with only the specific permission for the above function:
it allows a user to change the key for the database, but not to modify the provider configuration.

### Creating tables

To create an encrypted table or modify an existing table to be encrypted, simply use `USING tde_heap` in the `CREATE` / `ALTER TABLE` statement.

### Changing the pg_tde.inherit_global_keys setting

It is possible for users to use pg_tde with `inherit_global_keys=ON`, refer to global keys / keyrings in databases, and then change this setting to OFF.

In this case, existing references to global providers, or the global default principal key will remain working as before, but new references to the global scope can't be made.

### Using command line tools with encrypted WAL

TODO

## Typical setup scenarios

### Simple "one principal key" encryption

1. Installing the extension: `shared_preload_libraries` + `pg_tde.wal_encrypt`
2. `CREATE EXTENSION pg_tde;` in `template1`
3. Adding a global key provider
4. Adding a default principal key using the same global provider
5. Changing the WAL encryption to use the default principal key
6. Optionally: setting the `default_table_access_method` to `tde_heap` so that tables are encrypted by default

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

Alternatively, `ALTER SYSTEM` is possible, but table creation in the database will fail if there's no principal key for the database, that has to be created first.

### Complete multi tenancy

1. Installing the extension: `shared_preload_libraries` +  `pg_tde.wal_encrypt` (that's not multi tenant currently)
2. `CREATE EXTENSION pg_tde;` in any database
2. Adding a global key provider for WAL
3. Changing the WAL encryption to use the proper global key provider

No default configuration:
key providers / principal keys are configured as a per database level, permissions are managed per database

Same note about `default_table_access_method` as above - but in a multi tenant setup, `ALTER SYSTEM` doesn't make much sense.
