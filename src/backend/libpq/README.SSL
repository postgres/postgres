src/backend/libpq/README.SSL

SSL
===

>From the servers perspective:


  Receives StartupPacket
           |
           |
 (Is SSL_NEGOTIATE_CODE?) -----------  Normal startup
           |                  No
           |
           | Yes
           |
           |
 (Server compiled with USE_SSL?) ------- Send 'N'
           |                       No        |
           |                                 |
           | Yes                         Normal startup
           |
           |
        Send 'S'
           |
           |
      Establish SSL
           |
           |
      Normal startup





>From the clients perspective (v6.6 client _with_ SSL):


      Connect
         |
         |
  Send packet with SSL_NEGOTIATE_CODE
         |
         |
  Receive single char  ------- 'S' -------- Establish SSL
         |                                       |
         | '<else>'                              |
         |                                  Normal startup
         |
         |
   Is it 'E' for error  ------------------- Retry connection
         |                  Yes             without SSL
         | No
         |
   Is it 'N' for normal ------------------- Normal startup
         |                  Yes
         |
   Fail with unknown

---------------------------------------------------------------------------

Ephemeral DH
============

Since the server static private key ($DataDir/server.key) will
normally be stored unencrypted so that the database backend can
restart automatically, it is important that we select an algorithm
that continues to provide confidentiality even if the attacker has the
server's private key.  Ephemeral DH (EDH) keys provide this and more
(Perfect Forward Secrecy aka PFS).

N.B., the static private key should still be protected to the largest
extent possible, to minimize the risk of impersonations.

Another benefit of EDH is that it allows the backend and clients to
use DSA keys.  DSA keys can only provide digital signatures, not
encryption, and are often acceptable in jurisdictions where RSA keys
are unacceptable.

The downside to EDH is that it makes it impossible to use ssldump(1)
if there's a problem establishing an SSL session.  In this case you'll
need to temporarily disable EDH (see initialize_dh()).
