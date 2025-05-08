# pg_waldump

[`pg_waldump` :octicons-link-external-16:](https://www.postgresql.org/docs/current/pgwaldump.html) is a tool to display a human-readable rendering of the Write-Ahead Log (WAL) of a PostgreSQL database cluster.

To read encrypted WAL records, `pg_waldump` supports the following additional arguments:

* `keyring_path`: the directory where keyring configuration files for WAL are stored. These files include:
  * `pg_tde.map`
  * `pg_tde.dat`
  * `pg_tde_keyrings`

!!! note

    `pg_waldump` will not decrypt WAL unless the `keyring_path` is set.
