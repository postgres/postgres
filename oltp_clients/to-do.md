## This week
- implement all supports in Epoxy. (7-1)
- see the effect of multi-leveling regarding YCSB. (2-3)
- User-injected delay to databases. (1-2)


## Investigate existing works


What is the txn requirement in existing works.
- read all codes from ad-hoc transactions, see how they use cross-engine transactions.
- See Epoxy code, see how they implement the varying benchmark.

## Implement Epoxy

Cross engine transaction support.

1. implement to the PostgreSQL
2. implement to ES
3. implement to MongoDB
4. implement to S3
5. implement to MySQL


See the effectiveness of operation reordering and multi-level atomic commit.


## Other Baselines

- Compare with Skeena? 
- Compare with XOpen/XA 
- Compare with Omid. 
- Bolt-on transactions
- Cherry Garcia protocol