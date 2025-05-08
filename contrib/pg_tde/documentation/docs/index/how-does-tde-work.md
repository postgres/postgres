# How TDE Works

To encrypt the data, two types of keys are used:

* **Internal encryption keys** to encrypt user data. They are stored internally, near the data that they encrypt.
* The **principal key** to encrypt database keys. It is kept separately from the database keys and is managed externally in the key management store.

!!! note

    For more information on managing and storing principal keys externally, see [Configure Global Key Provider](../global-key-provider-configuration/index.md).

You have the following options to store and manage principal keys externally:

* Use the HashiCorp Vault server. Only the back end KV Secrets Engine - Version 2 (API) is supported.
* Use the KMIP-compatible server. `pg_tde` has been tested with the [PyKMIP](https://pykmip.readthedocs.io/en/latest/server.html) server and [the HashiCorp Vault Enterprise KMIP Secrets Engine](https://www.vaultproject.io/docs/secrets/kmip).

The encryption process is the following:

![image](../_images/tde-flow.png)

When a user creates an encrypted table using `pg_tde`, a new random key is generated internally for that table and is encrypted using the AES-CBC cipher algorithm. This key is used to encrypt all data the user inserts in that table. Eventually the encrypted data gets stored in the underlying storage.

The internal key itself is encrypted using the principal key. The principal key is stored externally in the key management store.

Similarly when the user queries the encrypted table, the principal key is retrieved from the key store to decrypt the internal key. Then the same unique internal key for that table is used to decrypt the data, and unencrypted data gets returned to the user. So, effectively, every TDE table has a unique key, and each table key is encrypted using the principal key.

[Encrypted Data Scope](tde-encrypts.md){.md-button}
