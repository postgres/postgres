# FAQ

## Why do I need TDE?

- Compliance to security and legal regulations like General Data Protection Regulation (GDPR), Payment Card Industry Data Security Standard (PCI DSS), California Consumer Privacy Act (CCPA), Data Protection Act 2018 (DPA 2018) and others
- Encryption of backups. Even when an authorized person gets physical access to a backup, encryption ensures that the data remains unreadable and secure.
- Granular encryption of specific data sets and reducing the performance overhead that encryption brings. 
- Additional layer of security to existing security measures

## When and how should I use TDE?

If you are dealing with Personally Identifiable Information (PII), data encryption is crucial. Especially if you are involved in areas with strict regulations like:

* financial services where TDE helps to comply with PCI DSS 
* healthcare and insurance - compliance with HIPAA, HITECH, CCPA
* telecommunications, government and education to ensure data confidentiality.

Using TDE helps you avoid the following risks:

* Data breaches
* Identity theft that may lead to financial fraud and other crimes
* Reputation damage leading to loss of customer trust and business
* Legal consequences and financial losses for non-compliance with data protection regulations
* Internal threats by misusing unencrypted sensitive data 

If to translate sensitive data to files stored in your database, these are user data in tables, temporary files, WAL files. TDE has you covered encrypting all these files.

`pg_tde` does not encrypt system catalogs yet. This means that statistics data and database metadata are not encrypted. The encryption of system catalogs is planned for future releases. 


## I use disk-level encryption. Why should I care about TDE?

Encrypting a hard drive encrypts all data, including system, application, and temporary files.

Full disk encryption protects your data from people who have physical access to your device and even if it is lost or stolen. However, it doesn't protect the data after system boot-up: the data is automatically decrypted when the system runs or when an authorized user requests it. 

Another point to consider is PCI DSS compliance for Personal Account Numbers (PAN) encryption.

* **PCI DSS 3.4.1** standards might consider disk encryption sufficient for compliance if you meet these requirements:

   * Separate the logical data access from the operating system authentication.

   * Ensure the decryption key is not linked to user accounts.

   Note that PCI DSS 3.4.1 is retiring on March 31, 2025. Therefore, consider switching to PCI DSS 4.0.

* **PCI DSS 4.0** standards consider using only disk and partition-level encryption not enough to ensure PAN protection. It requires an additional layer of security that `pg_tde` can provide. 

`pg_tde` focuses specifically on data files and offers more granular control over encrypted data. The data remains encrypted on disk during runtime and when you move it to another directory, another system or storage. An example of such data is backups. They remain encrypted when moved to the backup storage.

Thus, to protect your sensitive data, consider using TDE to encrypt it at the table level. Then use disk-level encryption to encrypt a specific volume where this data is stored, or the entire disk.

## Is TDE enough to ensure data security?

No. TDE is an additional layer to ensure data security. It protects data at rest. Consider introducing also these measures:

* Access control and authentication
* Strong network security like TLS
* Disk encryption
* Regular monitoring and auditing
* Additional data protection for sensitive fields (e.g., application-layer encryption)

## How does `pg_tde` make my data safe?

`pg_tde` uses two keys to encrypt data:

* Internal encryption keys to encrypt the data. These keys are stored internally in an encrypted format, in a single `$PGDATA/pg_tde` directory.
* Principal keys to encrypt internal encryption keys. These keys are stored externally, in the Key Management System (KMS). 

You can use the following KMSs:

* [HashiCorp Vault](https://developer.hashicorp.com/vault/docs/what-is-vault). `pg_tde` supports the KV secrets engine v2 of Vault. 
* [OpenBao](https://openbao.org/) implementation of Vault
* KMIP-compatible server. KMIP is a standardized protocol for handling cryptographic workloads and secrets management

HashiCorp Vault can also act as the KMIP server, managing cryptographic keys for clients that use the KMIP protocol. 

Here’s how encryption of data files works:

First, data files are encrypted with internal keys. Each file that has a different OID, has an internal key. For example, a table with 4 indexes will have 5 internal keys - one for the table and one for each index.	

The initial decision on what file to encrypt is based on the table access method in PostgreSQL. When you run a `CREATE` or `ALTER TABLE` statement with the `USING tde_heap` clause, the newly created data files are marked as encrypted, and then file operations encrypt/decrypt the data. Later, if an initial file is re-created as a result of a `TRUNCATE` or `VACUUM FULL` command, the newly created file inherits the encryption information and is either encrypted or not. 

The principal key is used to encrypt the internal keys. The principal key is stored in the key management store. When you query the table, the principal key is retrieved from the key store to decrypt the table. Then the internal key for that table is used to decrypt the data.

WAL encryption is done globally for the entire database cluster. All modifications to any database within a PostgreSQL cluster are written to the same WAL to maintain data consistency and integrity and ensure that PostgreSQL cluster can be restored to a consistent state. Therefore, WAL is encrypted globally. 

When you turn on WAL encryption, `pg_tde` encrypts entire WAL files starting from the first WAL write after the server was started with the encryption turned on.

The same 2-tier approach is used with WAL as with the table data: WAL pages are first encrypted with the internal key. Then the internal key is encrypted with the global principal key.

You can turn WAL encryption on and off so WAL can contain both encrypted and unencrypted data. The WAL encryption GUC variable influences only writes.

Whenever the WAL is being read (by the recovery process or tools), the decision on what should be decrypted is based solely on the metadata of WAL encryption keys.


## Should I encrypt all my data?

It depends on your business requirements and the sensitivity of your data. Encrypting all data is a good practice but it can have a performance impact. 

Consider encrypting only tables that store sensitive data. You can decide what tables to encrypt and with what key. The [Set up multi-tenancy](multi-tenant-setup.md) section in the documentation focuses on this approach.

We advise encrypting the whole database only if all your data is sensitive, like PII, or if there is no other way to comply with data safety requirements.

## What cipher mechanisms are used by `pg_tde`?

`pg_tde` currently uses a AES-CBC-128 algorithm. First the internal keys in the datafile are encrypted using the principal key with AES-CBC-128, then the file data itself is again encrypted using AES-CBC-128 with the internal key.

For WAL encryption, AES-CTR-128 is used.

The support of other encryption mechanisms such as AES256 is planned for future releases. Reach out to us with your requirements and usage scenarios of other encryption methods are needed.

## Is post-quantum encryption supported?

No, it's not yet supported. In our implementation we reply on OpenSSL libraries that don't yet support post-quantum encryption.

## Can I encrypt an existing table?

Yes, you can encrypt an existing table. Run the `ALTER TABLE` command as follows:

```
ALTER TABLE table_name SET ACCESS METHOD tde_heap;
```

Since the `SET ACCESS METHOD` command drops hint bits and this may affect the performance, we recommend to run the `SELECT count(*)` command. It checks every tuple for visibility and sets its hint bits. Read more in the [Changing existing table](test.md) section.

## Do I have to restart the database to encrypt the data?

You must restart the database in the following cases to apply the changes:

* after you enabled the `pg_tde` extension
* to turn on / off the WAL encryption

After that, no database restart is required. When you create or alter the table using the `tde_heap` access method, the files are marked as those that require encryption. The encryption happens at the storage manager level, before a transaction is written to disk. Read more about [how tde_heap works](table-access-method.md#how-tde_heap-works).

## What happens to my data if I lose a principal key?

If you lose encryption keys, especially, the principal key, the data is lost. That's why it's critical to back up your encryption keys securely and use the Key Management service for key management.

## Can I use `pg_tde` in a multi-tenant setup?

Multi-tenancy is the type of architecture where multiple users, or tenants, share the same resource. It can be a database, a schema or an entire cluster. 

In `pg_tde`, multi-tenancy is supported via a separate principal key per database. This means that a database owner can decide what tables to encrypt within a database. The same database can have both encrypted and non-encrypted tables.

To control user access to the databases, you can use role-based access control (RBAC).

WAL files are encrypted globally across the entire PostgreSQL cluster using the same encryption keys. Users don't interact with WAL files as these are used by the database management system to ensure data integrity and durability.

## Are my backups safe? Can I restore from them?

`pg_tde` encrypts data at rest. This means that data is stored on disk in an encrypted form. During a backup, already encrypted data files are copied from disk onto the storage. This ensures the data safety in backups.

Since the encryption happens on the database level, it makes no difference for your tools and applications. They work with the data in the same way.

To restore from an encrypted backup, you must have the same principal encryption key, which was used to encrypt files in your backup.  

## I'm using OpenSSL in FIPS mode and need to use `pg_tde`. Does `pg_tde` comply with FIPS requirements? Can I use my own FIPS-mode OpenSSL library with `pg_tde`?

Yes. `pg_tde` works with the FIPS-compliant version of OpenSSL, whether it is provided by your operating system or if you use your own OpenSSL libraries. If you use your own libraries, make sure they are FIPS certified.
