# pg_waldump

[`pg_waldump` :octicons-link-external-16:](https://www.postgresql.org/docs/current/pgwaldump.html) is a tool to display a human-readable rendering of the Write-Ahead Log (WAL) of a PostgreSQL database cluster.

!!! warning
    The WAL encryption feature is currently in beta and is not effective unless explicitly enabled. It is not yet production ready. **Do not enable this feature in production environments**.

To read encrypted WAL records, `pg_waldump` supports the following additional arguments:

* `keyring_path` is the directory where the keyring configuration files for WAL are stored. The following files are included:
    * `1664_keys`
    * `1664_providers`

!!! note
    `pg_waldump` does not decrypt WAL unless the `keyring_path` is set.
