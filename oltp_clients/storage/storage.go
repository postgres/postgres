package storage

import (
	"FC/configs"
	"context"
	"encoding/json"
	"fmt"
	"github.com/jackc/pgx/v4"
	"log"
	"strconv"
	"sync"
	"time"
)

// Shard maintains a Local kv-store and all information needed.
type Shard struct {
	shardID                 string
	mu                      *sync.Mutex
	txnPool                 sync.Map
	ctx                     context.Context
	LockWindowInjectedDelay time.Duration

	// In case of benchmark
	length           int
	tables           sync.Map // tables with a primary index for each table.
	secondaryIndexes sync.Map
	//concurrentTxnNum uint32
	log *LogManager

	// In case of MongoDB
	mdb *MongoDB

	// In case of PostgreSQL.
	db *SQLDB

	ruleBasedGlobal *RBGlobalStatus

	// In case of MySQL

	// In case of ElasticSearch

	// In case of GridFS

	// In case of GCS
}

func (c *Shard) GetID() string {
	return c.shardID
}

// AddTable add a new table into the shard.
func (c *Shard) AddTable(tableName string, attributeNum int) *Table {
	tab := &Table{tableName: tableName, attributesNum: attributeNum, autoIncreasingPrimary: 0}
	tab.primaryIndex = NewBTree(tableName + "-MainIndex")
	c.tables.Store(tableName, tab)
	return tab
}

// Clear all pending transactions and connections.
func (c *Shard) Clear() {
	switch c.ctx.Value("store").(string) {
	case configs.PostgreSQL:
		//c.db.mustExec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE pid != pg_backend_pid();")
		rows, err := c.db.pool.Query(c.db.ctx, "SELECT gid FROM pg_prepared_xacts;")
		if err != nil {
			panic(err)
		}
		for rows.Next() {
			var gid string
			if err := rows.Scan(&gid); err != nil {
				log.Fatal(err)
			}
			_, err := c.db.pool.Exec(c.db.ctx, fmt.Sprintf("ROLLBACK PREPARED '%s'", gid))
			if err != nil {
				log.Printf("Failed to rollback prepared transaction with gid %s: %v", gid, err)
			} else {
				fmt.Printf("Rolled back prepared transaction with gid %s\n", gid)
			}
		}
	default:
		// nothing to do.
	}
}

func newShardKV(shardID string, storeType string, delay time.Duration) *Shard {
	c := &Shard{
		shardID:                 shardID,
		mu:                      &sync.Mutex{},
		ctx:                     context.WithValue(context.Background(), "store", storeType),
		LockWindowInjectedDelay: delay,
	}
	if c.ctx.Value("store").(string) == configs.BenchmarkStorage {
		c.log = NewLogManager(shardID)
	} else if c.ctx.Value("store").(string) == configs.MongoDB {
		c.mdb = &MongoDB{}
		c.mdb.init(shardID[len(shardID)-3:])
	} else if c.ctx.Value("store").(string) == configs.PostgreSQL {
		c.db = &SQLDB{}
		c.ruleBasedGlobal = NewRBGlobalStatus()
		c.db.init()
		c.Clear()
	}
	return c
}

/* Interactive simple key-Value APIs.*/
// TODO: support delete and range scan with lock.

func (c *Shard) Insert(tableName string, key uint64, value *RowData) bool {
	if c.ctx.Value("store").(string) == configs.MongoDB {
		if !c.mdb.Insert(tableName, key, value) {
			return false
		}
	} else if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.db.Insert(tableName, key, value)
	}
	tab, ok := c.tables.Load(tableName)
	//configs.JPrint(tab)
	configs.Assert(ok, "the table does not exist")
	t, ok := tab.(*Table)
	configs.Assert(ok, "the loaded table metadata from kv.table is invalid")
	index := t.primaryIndex
	row := NewRowRecord(t, Key(key), Key(key))
	row.Data = value
	err := index.IndexInsert(Key(key), row)
	if err != nil && err != ErrAbort {
		panic(err)
	}
	return err == nil
}

func (c *Shard) Update(tableName string, key uint64, value *RowData) bool {
	if c.ctx.Value("store").(string) == configs.MongoDB {
		return c.mdb.Update(tableName, key, value)
	} else if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.db.Update(tableName, key, value)
	}
	tab, ok := c.tables.Load(tableName)
	configs.Assert(ok, "the table does not exist")
	index := tab.(*Table).primaryIndex
	row, err := index.IndexRead(Key(key))
	if err != nil {
		panic(err)
	}
	tempTxn := NewTxn(c.ctx)
	tempTxn.txnID = uint32(time.Now().UnixMicro() & 0x7fffffff)
	tempRow, err := tempTxn.AccessRow(row, TxnWrite)
	if err == nil {
		tempRow.Data = value
		tempTxn.TxnState = txnCommitted
		c.log.writeRedoLog4Txn(tempTxn)
		c.log.writeTxnState(tempTxn)
		tempTxn.Finish(true)
		return true
	} else if err == ErrAbort {
		tempTxn.TxnState = txnAborted
		c.log.writeRedoLog4Txn(tempTxn)
		c.log.writeTxnState(tempTxn)
		tempTxn.Finish(false)
		return false
	} else {
		panic(err)
		return false
	}
}

func (c *Shard) Read(tableName string, key uint64) (*RowData, bool) {
	if c.ctx.Value("store").(string) == configs.MongoDB {
		return c.mdb.Read(tableName, key)
	} else if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.db.Read(tableName, key)
	}
	tab, ok := c.tables.Load(tableName)
	configs.Assert(ok, "the table does not exist")
	index := tab.(*Table).primaryIndex
	row, err := index.IndexRead(Key(key))
	if err != nil {
		panic(err)
	}
	tempTxn := NewTxn(c.ctx)
	tempTxn.txnID = uint32(time.Now().UnixMicro() & 0x7fffffff)
	r, err := tempTxn.AccessRow(row, TxnRead)
	if err != nil && err != ErrAbort {
		panic(err)
	}
	tempTxn.Finish(err == nil)
	if err == nil {
		return r.Data, true
	} else {
		return nil, false
	}
}

/* Execution phase APIs for transactions. */

func (c *Shard) Begin(txnID uint32, opt []TXOpt) bool {
	configs.TPrintf("TXN" + strconv.FormatUint(uint64(txnID), 10) + ": transaction begun")
	_, ok := c.txnPool.Load(txnID)
	configs.Assert(!ok, "the previous transaction has not been finished yet (TID="+strconv.Itoa(int(txnID))+")")
	txn := NewTxn(c.ctx)
	txn.latch.Lock()
	defer txn.latch.Unlock()
	txn.txnID = txnID
	if c.ctx.Value("store").(string) == configs.PostgreSQL {
		var err error
		if configs.LockType == configs.RuleBasedLock {
			txn.ruleCC = NewRBTXStatus(c.ruleBasedGlobal, opt)
			if txn.ruleCC.isSSI {
				txn.sqlTX, err = c.db.Begin(pgx.Serializable)
			} else {
				txn.sqlTX, err = c.db.Begin(pgx.ReadUncommitted)
				if err != nil {
					panic(err)
				}
			}
		} else {
			if configs.DefaultIsolationLevel == pgx.ReadUncommitted {
				// read uncommitted for using system default isolation level.
				txn.sqlTX, err = c.db.BeginNoOp()
			} else {
				txn.sqlTX, err = c.db.Begin(configs.DefaultIsolationLevel)
			}
			if err != nil {
				panic(err)
			}
		}
		txn.isPrepared = false
		if err != nil {
			panic(err)
		}
	}
	c.txnPool.Store(txnID, txn)
	return true
}

func JPrint(v interface{}) {
	byt, _ := json.Marshal(v)
	fmt.Println(string(byt))
}

func (c *Shard) ReadTxn(tableName string, txnID uint32, key uint64) (*RowData, bool) {
	if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.ReadTxnPostgres(tableName, txnID, key)
	}
	tab, ok := c.tables.Load(tableName)
	configs.Assert(ok, "the table does not exist")
	index := tab.(*Table).primaryIndex
	row, err := index.IndexRead(Key(key))
	if err != nil {
		JPrint(tableName)
		JPrint(key)
		panic(err)
	}
	configs.TPrintf("TXN" + strconv.FormatUint(uint64(txnID), 10) + ": reading data on " +
		c.shardID + " " + tableName + ":" + strconv.Itoa(int(key)))
	v, ok := c.txnPool.Load(txnID)
	if !ok {
		configs.Warn(ok, "the transaction has been aborted.")
		configs.TPrintf("TXN" + strconv.FormatUint(uint64(txnID), 10) + ": transaction aborted on shard " +
			c.shardID + " " + tableName + ":" + strconv.Itoa(int(key)))
		// the transaction branch could have been aborted due to the abort of another participant.
		// in this case, we ignore it.
		return nil, false
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	configs.Assert(tx.txnID == txnID, "different transaction running")
	r, err := tx.AccessRow(row, TxnRead)
	if err != nil && err != ErrAbort {
		panic(err)
	}
	if err == nil {
		if c.ctx.Value("store").(string) == configs.MongoDB {
			// document level lock stored in memory.
			return c.mdb.Read(tableName, key)
		}
		return r.Data, true
	} else {
		configs.TxnPrint(uint64(txnID), fmt.Sprintf(" the txn update fail at updating %v-%v-%v", c.shardID, tableName, key))
		return nil, false
	}
}

func (c *Shard) UpdateTxn(tableName string, txnID uint32, key uint64, value *RowData) bool {
	if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.UpdateTxnPostgres(tableName, txnID, key, value)
	}
	tab, ok := c.tables.Load(tableName)
	configs.TPrintf("TXN" + strconv.FormatUint(uint64(txnID), 10) + ": update Value on shard " + c.shardID + " " + tableName + ":" + strconv.Itoa(int(key)) + ":" + value.String())
	configs.Assert(ok, "the table does not exist")
	index := tab.(*Table).primaryIndex
	row, err := index.IndexRead(Key(key))
	if err != nil {
		JPrint(tableName)
		JPrint(key)
		panic(err)
	}
	v, ok := c.txnPool.Load(txnID)
	if !ok {
		configs.Warn(ok, "the transaction has been aborted.")
		// the transaction branch could have been aborted due to the abort of another participant.
		// in this case, we ignore it.
		return false
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	configs.Assert(tx.txnID == txnID, "different transaction running")
	tempRow, err := tx.AccessRow(row, TxnWrite)
	if err == nil {
		if c.ctx.Value("store").(string) == configs.MongoDB {
			tempRow.Data = value
			return c.mdb.updateWithRollback(tableName, key, value, tempRow.Data)
		}
		tempRow.Data = value
		return true
	} else if err == ErrAbort {
		configs.TxnPrint(uint64(txnID), fmt.Sprintf(" the txn update fail at updating %v-%v-%v", c.shardID, tableName, key))
		return false
	} else {
		panic(err)
		return false
	}
}

func (c *Shard) RollBack(txnID uint32) bool {
	if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.RollBackPostgres(txnID)
	}
	v, ok := c.txnPool.Load(txnID)
	if !ok {
		configs.Warn(ok, "the transaction has been aborted.")
		return true
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	configs.Assert(tx.txnID == txnID, "different transaction running")
	if !tx.TryFinish() {
		return true
	}
	tx.TxnState = txnAborted
	if c.ctx.Value("store").(string) == configs.MongoDB {
		for i := tx.RowCnt - 1; i > 0; i-- {
			v := tx.Accesses[i]
			if v.AccessType == TxnWrite {
				for !c.mdb.rollBack(v.Local.FromTable.tableName, uint64(v.Local.PrimaryKey), v.Origin.Data) {
				}
			}
		}
	}
	tx.Finish(false)
	c.log.writeTxnState(tx)
	c.txnPool.Delete(txnID)
	return true
}

func (c *Shard) Commit(txnID uint32) bool {
	configs.TimeTrack(time.Now(), fmt.Sprintf("commit on shard %s", c.shardID), uint64(txnID))
	if c.ctx.Value("store").(string) == configs.PostgreSQL {
		return c.CommitPostgres(txnID)
	}
	v, ok := c.txnPool.Load(txnID)
	// the v is empty when the transaction has been committed before.
	// such case could occur when using EasyCommit with transit-before-decide or other protocols that could send commit
	// message to finished transactions.
	configs.Warn(ok, "the transaction has finished before commit on this node.")
	if !ok {
		return true
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	configs.Assert(tx.txnID == txnID, "different transaction running")
	if !tx.TryFinish() {
		return true
	}
	tx.TxnState = txnCommitted
	if tx.TxnState == txnExecution {
		// for a local transaction, there is no prepare phase that persists the redo logs.
		c.log.writeRedoLog4Txn(tx)
	}
	c.log.writeTxnState(tx)
	tx.Finish(true)
	c.txnPool.Delete(txnID)
	return true
}

/* APIs for distributed transactions. */

func (c *Shard) Prepare(txnID uint32) bool {
	v, ok := c.txnPool.Load(txnID)
	if !ok {
		configs.Warn(ok, "the transaction has been aborted.")
		// this could happen when the transaction exceeds crash timeout, the coordinator has asserted abort.
		return false
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	time.Sleep(c.LockWindowInjectedDelay)
	configs.Assert(ok, "the transaction has finished before commit on this node.")
	if tx.TxnState != txnExecution {
		// in G-PAC, this is a possible corner case that the transaction get committed/aborted before pre-write on a replica.
		return false
	}
	configs.Assert(tx.TxnState == txnExecution, "the transaction shall be in execution state before")
	configs.Assert(tx.txnID == txnID, "different transaction running")
	tx.TxnState = txnPrepare
	c.log.writeRedoLog4Txn(tx)
	c.log.writeTxnState(tx)
	if c.ctx.Value("store").(string) == configs.PostgreSQL {
		_, err := tx.sqlTX.Exec(c.db.ctx, fmt.Sprintf("PREPARE TRANSACTION 'TXN_%v_%v'", txnID, c.shardID))
		if err != nil {
			return false
		}
		tx.isPrepared = true
		//err = tx.sqlTX.Rollback(c.db.ctx)
		//if err != pgx.ErrTxClosed {
		//	panic("the transaction state shall be idle")
		//}
	}
	return true
}

func (c *Shard) PreCommit(txnID uint32) bool {
	v, ok := c.txnPool.Load(txnID)
	if !ok {
		configs.Warn(ok, "the transaction has been aborted.")
		return false
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	configs.Assert(ok, "the transaction has finished before commit on this node.")
	configs.Assert(tx.txnID == txnID, "different transaction running")
	//configs.JPrint(tx)
	configs.Assert(tx.TxnState == txnPrepare, "in 3PC, a transaction branch shall be prepared before pre-commit")
	tx.TxnState = txnPreCommit
	c.log.writeTxnState(tx)
	return true
}

// PreCommitAsync it is possible for a replica to receive pre-commit message before receiving prepare message.
// in this ignore it and does not reply the ACK.
func (c *Shard) PreCommitAsync(txnID uint32) bool {
	v, ok := c.txnPool.Load(txnID)
	if !ok {
		configs.Warn(ok, "the transaction has been aborted.")
		// the aborted transaction can also participant in the accepting process.
		return true
	}
	tx := v.(*DBTxn)
	tx.latch.Lock()
	defer tx.latch.Unlock()
	configs.Assert(ok, "the transaction has finished before commit on this node.")
	configs.Assert(tx.txnID == txnID, "different transaction running")
	if tx.TxnState != txnPrepare {
		return false
	}
	configs.Assert(tx.TxnState == txnPrepare, "in 3PC, a transaction branch shall be prepared before pre-commit")
	tx.TxnState = txnPreCommit
	c.log.writeTxnState(tx)
	return true
}
