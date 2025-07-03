# Limitations of pg_tde

* Keys in the local keyfile are stored **unencrypted**. For better security we recommend using the [Key management storage](../global-key-provider-configuration/index.md).
* System tables, which include statistics data and database statistics, are currently **not encrypted**.
* The WAL encryption feature is currently in beta and is not effective unless explicitly enabled. It is not yet production ready. **Do not enable this feature in production environments**.
* `pg_tde` RC 2 is incompatible with `pg_tde` Beta2 due to significant changes in code. There is no direct upgrade flow from one version to another. You must [uninstall](../how-to/uninstall.md) `pg_tde` Beta2 first and then [install](../install.md) and configure the new Release Candidate version.

!!! important
    This is the {{release}} version of the extension and **it is not meant for production use yet**. We encourage you to use it in testing environments and [provide your feedback](https://forums.percona.com/c/postgresql/pg-tde-transparent-data-encryption-tde/82).

[Versions and Supported PostgreSQL Deployments](supported-versions.md){.md-button}
