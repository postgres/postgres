## MockKV

A simple in-memory key-Value store used for experiment.

- Transaction supported: Serializable.
- Timeout for locks supported. (-1 for keep waiting)
- WAL supported for updates. (-1 for stable log, txnID for temporary logs)

You can just use MockKV with the following APIs.

#### APIs

| Exposed function |                         Explanation                          | Thread-safe |
| ---------------- | :----------------------------------------------------------: | :---------: |
| NewKV            | Create a new KV-store with size len representing the Local storage of the shard |      Y      |
| Update           |           API for KV-store: storage[key] = Value           |      Y      |
| Read             |               API for KV-store: storage[key]               |      Y      |
| Begin            | API for transaction: begin a transaction with given txnID  |      T      |
| ReadTxn          |   Read the Value and hold a readCnt lockStat for it with txnID.   |      T      |
| UpdateTxn        | Update the Value and hold a write lockStat for it with txnID.  |      T      |
| Commit           |   Release the locks held by txnID, and stable the logs.    |      T      |
| RollBack         |                Abandon the logs, and release                 |      T      |

- Y for thread-safe, T for thread-safe between different transactions.
 