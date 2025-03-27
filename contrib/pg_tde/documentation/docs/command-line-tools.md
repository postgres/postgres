# Command line tools

The `pg_tde` extension provides new command line tools and modifies some existing tools to work with encrypted WAL and tables.

## pg_tde_change_key_provider

A tool for modifying the configuration of a key provider, possibly also changing its type.

This tool edits the configuration files directly, ignoring permissions or running `postgres` processes.

Its only intended use is to fix servers that can't start up because of inaccessible key providers. 

For example, you restore from an old backup and the address of the key provider changed in the meantime. You can use this tool to correct the configuration, allowing the server to start up.

Use this tool **only when the server is offline.** To modify the key provider configuration when the server is up and running, use the [`pg_tde_change_key_provider_<type>`](functions.md#change-an-existing-provider) SQL functions.

### Usage

To modify the key provider configuration, specify all parameters depending on the provider type in the same way as you do when using the [`pg_tde_change_key_provider_<type>`](functions.md#change-an-existing-provider) SQL functions.
 
The general syntax is as follows:

```
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> <new_provider_type> <provider_parameters...>
```

where :

* [optional] `<datadir>` is the data directory. When not specified, `pg_tde` uses the `$PGDATA` value.
* `<provider_name>` is the name you assigned to the key provider
* `<new_provider_type>` can be a `file`, `vault` or `kmip`.
* `<dbOid>`


Depending on the provider type, the additional parameters are:

```
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> file <filename>
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> vault <token> <url> <mount_path> [<ca_path>]
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> kmip <host> <port> <cert_path> [<ca_path>]
```

## pg_waldump

[`pg_waldump` :octicons-link-external-16:](https://www.postgresql.org/docs/current/pgwaldump.html) is a tool to display a human-readable rendering of the write-ahead log of a PostgreSQL database cluster. 

To read encrypted WAL records, `pg_waldump` supports the following additional arguments:

* `keyring_path`: the directory where keyring config files for WAL are stored. These files are: 

   * `pg_tde.map`, 
   * `pg_tde.dat`, 
   * `pg_tde_keyrings` 

`pg_waldump` will not try to decrypt WAL if the `keyring_path`  is not set.

## pg_checksums

[`pg_checksums` :octicons-link-external-16:](https://www.postgresql.org/docs/current/app-pgchecksums.html) cannot calculate checksums for encrypted files.
It skips encrypted files and reports this in the output.
