# pg_tde_change_key_provider

A tool for modifying the configuration of a key provider, possibly also changing its type.

This tool edits the configuration files directly, ignoring permissions or running `postgres` processes.

Its only intended use is to fix servers that can't start up because of inaccessible key providers.

For example, you restore from an old backup and the address of the key provider changed in the meantime. You can use this tool to correct the configuration, allowing the server to start up.

<i warning>:material-information: Warning:</i> Use this tool **only when the server is offline.** To modify the key provider configuration when the server is up and running, use the [`pg_tde_change_(global/database)_key_provider_<type>`](../functions.md#change-an-existing-provider) SQL functions.

## Example usage

To modify the key provider configuration, specify all parameters depending on the provider type in the same way as you do when using the [`pg_tde_change_(global/database)_key_provider_<type>`](../functions.md#change-an-existing-provider) SQL functions.

The general syntax is as follows:

```sh
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> <new_provider_type> <provider_parameters...>
```

## Parameter description

* [optional] `<datadir>` is the data directory.`pg_tde` uses the `$PGDATA` environment variable if this is not specified
* `<provider_name>` is the name you assigned to the key provider
* `<new_provider_type>` can be a `file`, `vault` or `kmip`
* `<dbOid>`

Depending on the provider type, the additional parameters are:

```sh
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> file <filename>
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> vault-v2 <url> <mount_path> <token_path> [<ca_path>]
pg_tde_change_key_provider [-D <datadir>] <dbOid> <provider_name> kmip <host> <port> <cert_path> <key_path> [<ca_path>] 
```
