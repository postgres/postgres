# Benefits of pg_tde

The benefits of using `pg_tde` are outlined below for different users and organizations.

## Benefits for organizations

* **Data safety:** Prevents unauthorized access to stored data, even if backup files or storage devices are stolen or leaked.
* **Enterprise-ready Architecture:** Supports both single and multi-tenancy, giving flexibility for SaaS providers or internal multi-user systems.

## Benefits for DBAs and engineers

* **Granular control:** Encrypt specific tables or databases instead of the entire system, reducing performance overhead.
* **Operational simplicity:** Works transparently without requiring major application changes.
* **Defense in depth:** Adds another layer of protection to existing controls like TLS (encryption in transit), access control, and role-based permissions.

When combined with the external Key Management Systems (KMS), `pg_tde` enables centralized control, auditing, and rotation of encryption keys—critical for secure production environments.

!!! admonition "See also"

    You can find more information on Transparent Data Encryption (TDE) in [this article](https://www.percona.com/blog/transparent-data-encryption-tde/).
    
[Learn how pg_tde works :material-arrow-right:](how-does-tde-work.md){.md-button}
