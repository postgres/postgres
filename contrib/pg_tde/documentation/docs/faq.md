# FAQ

## Why do I need TDE?

- Compliance to security and legal regulations like GDPR, PCI DSS and others
- Encryption of backups
- Granular encryption of specific data sets and reducing the performance overhead that encryption brings
- Additional layer of security to existing security measures

## I use disk-level encryption. Why should I care about TDE?

Encrypting a hard drive encrypts all data including system and application files that are there. However, disk encryption doesn’t protect your data after the boot-up of your system. During runtime, the files are decrypted with disk-encryption.

TDE focuses specifically on data files and offers a more granular control over encrypted data. It also ensures that files are encrypted on disk during runtime and when moved to another system or storage.

Consider using TDE and storage-level encryption together to add another layer of data security

## Is TDE enough to ensure data security?

No. TDE is an additional layer to ensure data security. It protects data at rest. Consider introducing also these measures:

* Access control and authentication
* Strong network security like TLS
* Disk encryption
* Regular monitoring and auditing
* Additional data protection for sensitive fields (e.g., application-layer encryption)

## What happens to my data if I lose a principal key?

If you lose encryption keys, especially, the principal key, the data is lost. That's why it's critical to back up your encryption keys securely.