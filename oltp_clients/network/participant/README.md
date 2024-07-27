## Manager

The participant component for FCff Protocol.
- It serves as a bridge between request fullNode coordinator and Local store.
- One `ParticipantStmt` with one `Shard` is needed for each node.

The unit tests for this component is used for development. If you want to run them, please change to Local test setup

#### APIs

|          API          |                    Explanation                     | Thread-safe |
| :-------------------: | :------------------------------------------------: | :---------: |
|         Main          |  Start the participant node with Args and config file.  |      Y      |
|     Break/Recover     | Used for Local test to simulate the crash failure. |      Y      |
|        PreRead        | Handles a readCnt only transaction and return values. |      T      |
| PreWrite/Commit/Abort |                  Common handlers                   |      T      |
|         PreCommit         |                    3PC handler                     |      T      |
|        ST1        |     For FCff propose phase, return the results      |      T      |


- Y for thread-safe, T for thread-safe between transactions.
