# GUC Variables

The `pg_tde` extension provides GUC variables to configure the behaviour of the extension:

## pg_tde.wal_encrypt

**Type** - boolean <br>
**Default** - off

A `boolean` variable that controls if WAL writes are encrypted or not.

Changing this variable requires a server restart, and can only be set at the server level.

WAL encryption is controlled globally. If enabled, all WAL writes are encrypted in the entire PostgreSQL cluster.

This variable only controls new writes to the WAL, it doesn't affect existing WAL records.

`pg_tde` is always capable of reading existing encrypted WAL records, as long as the keys used for the encryption are still available.

Enabling WAL encryption requires a configured global principal key. Refer to the [WAL encryption configuration](wal-encryption.md) documentation for more information.

## pg_tde.enforce_encryption

**Type** - boolean <br>
**Default** - off

A `boolean` variable that controls if the creation of new, not encrypted tables is allowed.

If enabled, `CREATE TABLE` statements will fail unless they use the `tde_heap` access method.

Similarly, `ALTER TABLE <x> SET ACCESS METHOD` is only allowed, if the access method is `tde_heap`.

Other DDL operations are still allowed. For example other `ALTER` commands are allowed on unencrypted tables, as long as the access method isn't changed.

You can set this variable at the following levels:

* global - for the entire PostgreSQL cluster
* database - for specific databases
* user - for specific users
* session - for the current session

Setting or changing the value requires superuser permissions. For examples, see the [Encryption Enforcement](how-to/enforcement.md) topic.

## pg_tde.inherit_global_providers

**Type** - boolean <br>
**Default** - on

A `boolean` variable that controls if databases can use global key providers for storing principal keys.

If disabled, functions that change the key providers can only work with database local key providers.

In this case, the default principal key, if set, is also disabled.

You can set this variable at the following levels:

* global - for the entire PostgreSQL cluster
* database - for specific databases
* user - for specific users
* session - for the current session

!!! note
    Setting this variable doesn't affect existing uses of global keys. It only prevents the creation of new principal keys using global providers.
