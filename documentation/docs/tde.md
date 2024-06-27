# What is Transparent Data Encryption (TDE)

Transparent Data Encryption offers encryption at the file level and solves the problem of protecting data at rest. The encryption is transparent for users allowing them to access and manipulate the data and not to worry about the encryption process.

## How does it work?

To encrypt the data, two types of keys are used:

* Database keys to encrypt user data. These are stored internally, near the data that they encrypt.
* The principal key to encrypt database keys. It is kept separately from the database keys and is managed externally. 

`pg_tde` is integrated with HashiCorp Vault server to store and manage principal keys. Only the back end KV Secrets Engine - Version 2 (API) is supported.

The encryption process is the following:

![image](_images/tde-flow.png)

When a user creates an encrypted table using `pg_tde`, a new random key is generated for that table. This key is used to encrypt all data the user inserts in that table. Eventually the encrypted data gets stored in the underlying storage. 

The table itself is encrypted using the principal key. The principal key is stored externally in the Vault key management store. 

Similarly when the user queries the encrypted table, the principal key is retrieved from the key store to decrypt the table. Then the same unique internal key for that table is used to decrypt the data, and unencrypted data gets returned to the user. So, effectively, every TDE table has a unique key, and each table key is encrypted using the principal key.

## Why do you need TDE?

Using TDE has the following benefits:

* For organizations:
   
    - Ensure data safety when at rest and in motion
    - Comply with security standards like HIPAA, PCI DSS, SOC 2, ISO 27001

* For DBAs:
   
    - Allows defining what to encrypt in the table and with what key
    - Encryption on storage level is not a must to provide data safety. However, using TDE and storage-level encryption together adds another layer of data security

!!! admonition "See also"

    Percona Blog: [Transparent Data Encryption (TDE)](https://www.percona.com/blog/transparent-data-encryption-tde/)