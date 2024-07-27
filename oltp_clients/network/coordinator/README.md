#### APIs

|       API        |                      Explanation                      | Thread-safe |
| :--------------: |:-----------------------------------------------------:| :---------: |
|       Main       | Start the coordinator node with Args and config file. |      Y      |
| NewTX |                 Create a transaction.                 |      T      |
|     PreRead      |    Pre-read the data with a read-only transaction.    |      T      |
|    SubmitTxn     |       Submit a transaction with dedicated ACP.        |      T      |

- Y for thread-safe, T for thread-safe between transactions.


