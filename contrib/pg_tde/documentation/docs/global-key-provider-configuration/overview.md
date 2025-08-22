# Key management overview

In production environments, storing encryption keys locally on the PostgreSQL server can introduce security risks. To enhance security, `pg_tde` supports integration with external Key Management Systems (KMS) through a Global Key Provider interface.

This section describes how you can configure `pg_tde` to use the local and external key providers.
To use an external KMS with `pg_tde`, follow these two steps:

1. Configure a Key Provider
2. Set the [Global Principal Key](set-principal-key.md)

!!! note
     While key files may be acceptable for **local** or **testing environments**, KMS integration is the recommended approach for production deployments.

`pg_tde` has been tested with the following key providers:

| KMS Provider       | Description                                           | Documentation |
|--------------------|-------------------------------------------------------|---------------|
| **KMIP**           | Standard Key Management Interoperability Protocol.    | [Configure KMIP →](kmip-server.md) |
| **Vault**          | HashiCorp Vault integration (KV v2 API, KMIP engine). | [Configure Vault →](vault.md) |
| **Fortanix**       | Fortanix DSM key management.                          | [Configure Fortanix →](kmip-fortanix.md) |
| **Thales**         | Thales CipherTrust Manager and DSM.                   | [Configure Thales →](kmip-thales.md) |
| **OpenBao**        | Community fork of Vault, supporting KV v2.            | [Configure OpenBao →](kmip-openbao.md) |
| **Keyring file** *(not recommended)* | Local key file for dev/test only.                  | [Configure keyring file →](keyring.md) |
