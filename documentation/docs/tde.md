# What is Transparent Data Encryption (TDE)

Transparent Data Encryption is a technology to protect data at rest. The encryption process happens transparently in the background, without affecting database operations. Data is automatically encrypted as it's written to the disk and decrypted as it's read, all in real-time. Users and applications interact with the data as usual without noticing any difference.

## How does it work?

To encrypt the data, two types of keys are used:

* Internal encryption keys to encrypt user data. They are stored internally, near the data that they encrypt.
* The principal key to encrypt database keys. It is kept separately from the database keys and is managed externally in the key management store. 

You have the following options to store and manage principal keys externally:

* Use the HashiCorp Vault server. Only the back end KV Secrets Engine - Version 2 (API) is supported.
* Use the KMIP-compatible server. `pg_tde` has been tested with the [PyKMIP](https://pykmip.readthedocs.io/en/latest/server.html) server and [the HashiCorp Vault Enterprise KMIP Secrets Engine](https://www.vaultproject.io/docs/secrets/kmip).

The encryption process is the following:

![image](_images/tde-flow.png)

When a user creates an encrypted table using `pg_tde`, a new random key is generated for that table using the AES128 (AES-ECB) cipher algorithm. This key is used to encrypt all data the user inserts in that table. Eventually the encrypted data gets stored in the underlying storage. 

The table itself is encrypted using the principal key. The principal key is stored externally in the key management store. 

Similarly when the user queries the encrypted table, the principal key is retrieved from the key store to decrypt the table. Then the same unique internal key for that table is used to decrypt the data, and unencrypted data gets returned to the user. So, effectively, every TDE table has a unique key, and each table key is encrypted using the principal key.

## Why do you need TDE?

Using TDE has the following benefits:

* For organizations:
   
    - Ensure data safety when it is stored on disk and in backups
    - Comply with security and legal standards like HIPAA, PCI DSS, SOC 2, ISO 27001

* For DBAs:
   
    - Granular encryption of specific tables and reducing the performance overhead that encryption brings
    - Additional layer of security to existing security measures like storage-level encryption, data encryption in transit using TLS, access control and more.

!!! admonition "See also"

    Percona Blog: [Transparent Data Encryption (TDE)](https://www.percona.com/blog/transparent-data-encryption-tde/)