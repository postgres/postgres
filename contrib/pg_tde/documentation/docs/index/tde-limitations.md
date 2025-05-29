# Limitations of pg_tde

* Keys in the local keyfile are stored unencrypted. For better security we recommend using the Key management storage.
* System tables are currently not encrypted. This means that statistics data and database metadata are currently not encrypted.

* `pg_rewind` doesn't work with encrypted WAL for now. We plan to fix it in future releases.
* `pg_tde` Release candidate is incompatible with `pg_tde`Beta2 due to significant changes in code. There is no direct upgrade flow from one version to another. You must [uninstall](../how-to/uninstall.md) `pg_tde` Beta2 first and then [install](../install.md) and configure the new Release Candidate version.

!!! important
    This is the {{release}} version of the extension and **it is not meant for production use yet**. We encourage you to use it in testing environments and [provide your feedback](https://forums.percona.com/c/postgresql/pg-tde-transparent-data-encryption-tde/82).

[Versions and Supported PostgreSQL Deployments](supported-versions.md){.md-button}
