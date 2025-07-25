# Limitations of pg_tde

The following are current limitations of `pg_tde`:

* System tables, which include statistics data and database statistics, are currently **not encrypted**.
* The WAL encryption feature is currently in beta and is not effective unless explicitly enabled. It is not yet production ready. **Do not enable this feature in production environments**.

[View the versions and supported deployments :material-arrow-right:](supported-versions.md){.md-button}
