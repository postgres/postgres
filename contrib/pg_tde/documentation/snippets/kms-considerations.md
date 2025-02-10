## Considerations

You can use the following options to manage encryption keys:

* Use the Key Management Store (KMS). This is the recommended approach. `pg_tde` supports the following KMS:

    * HashiCorp Vault as the key/value secrets engine version 2 with secret versioning
    * HashiCorp Vault as the KMIP server. The KMIP server is part of Vault Enterprise and requires a license
    * OpenBao as the open-source alternative to HashiCorp Vault KMIP 
    * A KMIP-compatible server. For testing and development purposes you can use PyKMIP

    The KMS configuration is out of scope of this document. We assume that you have the KMS up and running. For the `pg_tde` configuration, you need the following information:  

    === "Vault secrets engine"  

        * The secret access token to the Vault server
        * The URL to access the Vault server
        * (Optional) The CA file used for SSL verification  
    
    === "KMIP server"

        * The hostname or IP address of the KMIP server.
        * The valid certificates issued by the key management appliance.

* Use the local keyfile. Use the keyfile only development and testing purposes since the keys are stored unencrypted.