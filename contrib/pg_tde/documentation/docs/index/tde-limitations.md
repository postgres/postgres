# Limitations of pg_tde

* Keys in the local keyfile are stored unencrypted. For better security we recommend using the Key management storage.
* System tables are currently not encrypted. This means that statistics data and database metadata are currently not encrypted.

* `pg_rewind` doesn't work with encrypted WAL for now. We plan to fix it in future releases.
* `pg_tde` Release candidate is incompatible with `pg_tde`Beta2 due to significant changes in code. There is no direct upgrade flow from one version to another. You must [uninstall](../how-to/uninstall.md) `pg_tde` Beta2 first and then [install](../install.md) and configure the new Release Candidate version.

[Versions and supported PostgreSQL deployments](supported-versions.md){.md-button}
