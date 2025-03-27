# pg_tde MVP (2023-12-12)

The Minimum Viable Product (MVP) version of `pg_tde` introduces the following functionality:

* Encryption of heap tables, including TOAST
* Encryption keys are stored either in Hashicorp Vault server or in local keyring file (for development) 
* The key storage is configurable via separate JSON configuration files
* Replication support