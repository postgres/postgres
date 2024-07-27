package storage

import (
	"FC/configs"
	"context"
	"fmt"
	"github.com/jackc/pgx/v4"
	"github.com/jackc/pgx/v4/pgxpool"
	"log"
	"strconv"
)

type SQLDB struct {
	ctx       context.Context
	fromTable *Table
	pool      *pgxpool.Pool
}

type YCSBDataSQL struct {
	Key   string   `json:"key"`
	Value *RowData `json:"value"`
}

func (c *SQLDB) tryExec(sql string) {
	_, _ = c.pool.Exec(c.ctx, sql)
}

func (c *SQLDB) mustExec(sql string) {
	_, err := c.pool.Exec(c.ctx, sql)
	if err != nil {
		panic(err)
	}
}

func (c *SQLDB) init() {
	var err error
	c.ctx = context.TODO()
	config, err := pgxpool.ParseConfig("postgres://hexiang:flexi@localhost:5432/ycsb?sslmode=disable")
	config.MaxConns = 1000
	c.pool, err = pgxpool.ConnectConfig(context.Background(), config)
	if err != nil {
		log.Fatalf("Unable to connect to database: %v\n", err)
	}
	c.mustExec("ALTER SYSTEM SET max_connections = 1000")
	c.mustExec("ALTER SYSTEM SET shared_buffers = '2GB'")
	//c.mustExec("ALTER SYSTEM SET wal_level = 'minimal'")
	c.mustExec("ALTER SYSTEM SET fsync = 'off'")
	c.mustExec("ALTER SYSTEM SET full_page_writes = 'on'")
	if configs.LockType == configs.Native2PL {
		c.mustExec("ALTER SYSTEM SET default_cc_strategy = 's2pl'")
	} else if configs.LockType == configs.LearnedCC {
		c.mustExec("ALTER SYSTEM SET default_cc_strategy = 'learned'")
	} else if configs.LockType == configs.NoWait2PL {
		c.mustExec("ALTER SYSTEM SET default_cc_strategy = 's2plnw'")
	} else {
		c.mustExec("ALTER SYSTEM SET default_cc_strategy = 'ssi'")
	}
	c.mustExec("SELECT pg_reload_conf()")
	c.tryExec("DROP TABLE IF EXISTS YCSB_MAIN")
	c.tryExec("CREATE TABLE YCSB_MAIN (key VARCHAR(255) PRIMARY KEY,  value VARCHAR(255))")
	if configs.LockType == configs.WaitDie2PL {
		c.tryExec("DROP TABLE IF EXISTS LOCK_TAB")
		c.tryExec("CREATE UNLOGGED TABLE LOCK_TAB (key VARCHAR(255) PRIMARY KEY,  wts INT, rts INT)")
	}
}

func (c *SQLDB) Insert(tableName string, key uint64, value *RowData) bool {
	rec := YCSBDataSQL{Key: strconv.FormatUint(key, 10), Value: value}
	_, err := c.pool.Exec(c.ctx, "insert into YCSB_MAIN (key, value) values ($1, $2)", rec.Key, rec.Value.String())
	if err != nil {
		panic(err)
	}
	if configs.LockType == configs.WaitDie2PL {
		_, err = c.pool.Exec(c.ctx, "insert into LOCK_TAB (key, rts) values ($1, 0)", rec.Key)
		if err != nil {
			panic(err)
		}
	}
	return err == nil
}

func (c *SQLDB) Update(tableName string, key uint64, value *RowData) bool {
	rec := YCSBDataSQL{Key: strconv.FormatUint(key, 10), Value: value}
	_, err := c.pool.Exec(c.ctx, "update YCSB_MAIN set value = $2 where key = $1", rec.Key, rec.Value.String())
	return err == nil
}

func (c *SQLDB) Read(tableName string, key uint64) (*RowData, bool) {
	id := strconv.FormatUint(key, 10)
	res := RowData{}
	var value string
	err := c.pool.QueryRow(c.ctx, "select value from YCSB_MAIN where key = $1", id).Scan(&value)
	if err != nil {
		return nil, false
	}
	res.Length = 10
	res.Value = make([]interface{}, 10)
	res.fromTable = c.fromTable
	res.Value[0] = value
	return &res, err == nil
}

func (c *SQLDB) PackValue(value string) *RowData {
	res := RowData{}
	res.Length = 10
	res.Value = make([]interface{}, 10)
	res.fromTable = c.fromTable
	res.Value[0] = value
	return &res
}

func (c *SQLDB) ReadTx(ctx context.Context, tx pgx.Tx, tableName string, key uint64) (*RowData, bool) {
	id := strconv.FormatUint(key, 10)
	var value string
	var err error
	LockType := ctx.Value("LockType").(string)
	if LockType == configs.NoLock || LockType == configs.Native2PL || LockType == configs.NoWait2PL ||
		LockType == configs.LearnedCC {
		err = tx.QueryRow(c.ctx, "select value from YCSB_MAIN where key = $1", id).Scan(&value)
	} else {
		//if LockType == configs.NoWait2PL {
		//	err = tx.QueryRow(c.ctx, "select value from YCSB_MAIN where key = $1 for share nowait", id).Scan(&value)
		//} else if LockType == configs.NoWait2PL {
		//	var rts int
		//	err = tx.QueryRow(c.ctx, "select "+
		//		"rts from LOCK_TAB where key = $1", id).Scan(&rts)
		//	if err != nil {
		//		panic(err)
		//	}
		//	if ctx.Value("tid").(uint32) < uint32(rts) {
		//		// if requester is younger than holder, abort it directly
		//		//panic(fmt.Sprintf("impossible case %v and %v", ctx.Value("tid").(uint32), uint32(rts)))
		//		return nil, false
		//	} else {
		//		_, err = tx.Exec(c.ctx, "update LOCK_TAB set rts = $2 where key = $1 and rts < $2", id, ctx.Value("tid").(uint32))
		//		if err != nil {
		//			//println(rts)
		//			panic(err)
		//		}
		//	}
		//	err = tx.QueryRow(c.ctx, "select value from YCSB_MAIN where key = $1 for share", id).Scan(&value)
		//} else
		if LockType == configs.OCC {
			err = tx.QueryRow(c.ctx, "select value from YCSB_MAIN where key = $1", id).Scan(&value)
		} else {
			panic("invalid lock type")
		}
	}
	if err != nil {
		return nil, false
	}
	return c.PackValue(value), err == nil
}

func (c *SQLDB) Begin(iso pgx.TxIsoLevel) (pgx.Tx, error) {
	return c.pool.BeginTx(c.ctx, pgx.TxOptions{IsoLevel: iso})
}

func (c *SQLDB) BeginNoOp() (pgx.Tx, error) {
	return c.pool.BeginTx(c.ctx, pgx.TxOptions{})
}

func (c *SQLDB) UpdateTX(ctx context.Context, tx pgx.Tx, tableName string, key uint64, value *RowData) (*RowData, bool) {
	rec := YCSBDataSQL{Key: strconv.FormatUint(key, 10), Value: value}
	var err error
	var oldValue string
	LockType := ctx.Value("LockType").(string)
	if LockType == configs.NoLock || LockType == configs.Native2PL || LockType == configs.NoWait2PL ||
		LockType == configs.LearnedCC {
		_, err = tx.Exec(c.ctx, "update "+
			"YCSB_MAIN set value = $2 where key = $1", rec.Key, rec.Value.String())
	} else {
		//if LockType == configs.NoWait2PL {
		//	err = tx.QueryRow(c.ctx, "select "+
		//		"value from YCSB_MAIN where key = $1 for update nowait", rec.Key).Scan(&oldValue)
		//} else if LockType == configs.WaitDie2PL {
		//	panic("wait die has some error, to be implemented")
		//	//var rts int
		//	//err = tx.QueryRow(c.ctx, "select "+
		//	//	"rts from LOCK_TAB where key = $1", rec.Key).Scan(&rts)
		//	//if err != nil {
		//	//	panic(err)
		//	//}
		//	//if ctx.Value("tid").(uint32) < uint32(rts) {
		//	//	// if requester is younger than holder, abort it directly
		//	//	//panic(fmt.Sprintf("impossible case %v and %v", ctx.Value("tid").(uint32), uint32(rts)))
		//	//	return nil, false
		//	//} else {
		//	//	_, err = tx.Exec(c.ctx, "update LOCK_TAB set rts = $2 where key = $1 and rts < $2", rec.Key, ctx.Value("tid").(uint32))
		//	//}
		//} else if LockType == configs.OCC {
		//	if configs.NoBlindWrite && configs.StoredProcedure {
		//		// update --> read modify write.
		//		err = tx.QueryRow(c.ctx, "select value from "+
		//			"YCSB_MAIN where key = $1", rec.Key).Scan(&oldValue)
		//		if err != nil {
		//			//panic(err)
		//			return nil, false
		//		}
		//	}
		//} else {
		//	panic("invalid lock type")
		//}
		//if err != nil {
		//	//panic(err)
		//	return nil, false
		//}
		if LockType != configs.OCC {
			// In OCC, operations in write set are only flushed to DB after validate.
			_, err = tx.Exec(c.ctx, "update "+
				"YCSB_MAIN set value = $2 where key = $1", rec.Key, rec.Value.String())
		}
	}
	if configs.NoBlindWrite && configs.StoredProcedure {
		return c.PackValue(oldValue), err == nil
	} else {
		return nil, err == nil
	}
}

func (c *Shard) ReadTxnPostgres(tableName string, txnID uint32, key uint64) (*RowData, bool) {
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
	var ctx context.Context
	if configs.LockType == configs.RuleBasedLock {
		ctx = context.WithValue(c.ctx, "LockType", tx.ruleCC.GetNext(&TXOpt{Type: UpdateOpt, Key: key}))
	} else {
		ctx = context.WithValue(c.ctx, "LockType", configs.LockType)
	}
	LockType := ctx.Value("LockType").(string)
	//if LockType != configs.Native2PL {
	//	//println(tx.ruleCC.)
	//}
	//if LockType == configs.WaitDie2PL {
	//	ctx = context.WithValue(ctx, "tid", tx.txnID)
	//}
	value, ok := c.db.ReadTx(ctx, tx.sqlTX, tableName, key)
	if LockType == configs.OCC && ok {
		tx.Accesses[tx.RowCnt] = &txnAccess{AccessType: TxnRead,
			Local: &RowRecord{PrimaryKey: Key(key), Data: value}}
		tx.RowCnt++
	}
	if configs.LockType == configs.RuleBasedLock {
		tx.ruleCC.Report(&TXOpt{Type: ReadOpt, Key: key}, ok, ctx.Value("LockType").(string))
	}
	return value, ok
}

func (c *Shard) UpdateTxnPostgres(tableName string, txnID uint32, key uint64, value *RowData) bool {
	configs.TPrintf("TXN" + strconv.FormatUint(uint64(txnID), 10) + ": update Value on shard " + c.shardID + " " + tableName + ":" + strconv.Itoa(int(key)) + ":" + value.String())
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
	var ctx context.Context
	if configs.LockType == configs.RuleBasedLock {
		ctx = context.WithValue(c.ctx, "LockType", tx.ruleCC.GetNext(&TXOpt{Type: UpdateOpt, Key: key}))
	} else {
		ctx = context.WithValue(c.ctx, "LockType", configs.LockType)
	}
	LockType := ctx.Value("LockType").(string)
	if LockType == configs.OCC {
		tx.Accesses[tx.RowCnt] = &txnAccess{AccessType: TxnWrite,
			Local: &RowRecord{PrimaryKey: Key(key), Data: value}}
		tx.RowCnt++
	}
	//if LockType == configs.WaitDie2PL {
	//	ctx = context.WithValue(ctx, "tid", tx.txnID)
	//}
	var val *RowData = nil
	val, ok = c.db.UpdateTX(ctx, tx.sqlTX, tableName, key, value)
	if configs.LockType == configs.RuleBasedLock && ok {
		tx.ruleCC.Report(&TXOpt{Type: ReadOpt, Key: key}, ok, ctx.Value("LockType").(string))
	}
	if val != nil && ok && LockType == configs.OCC {
		tx.Accesses[tx.RowCnt] = &txnAccess{AccessType: TxnRead,
			Local: &RowRecord{PrimaryKey: Key(key), Data: val}}
		tx.RowCnt++
	}
	return ok
}

func (c *Shard) RollBackPostgres(txnID uint32) bool {
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
	tx.latch.Lock()
	defer tx.latch.Unlock()
	tx.TxnState = txnAborted
	if tx.isPrepared {
		_, err := tx.sqlTX.Exec(c.ctx, fmt.Sprintf("ROLLBACK PREPARED 'TXN_%v_%v'", txnID, c.shardID))
		if err != nil {
			panic(err)
		}
		err = tx.sqlTX.Rollback(c.ctx)
		//if err != pgx.ErrTxClosed {
		//	// release connection
		//	panic("double commit must be idle")
		//}
	} else {
		err := tx.sqlTX.Rollback(c.ctx)
		if configs.LockType == configs.RuleBasedLock {
			tx.ruleCC.Finish(false)
		}
		if err != nil {
			JPrint(tx)
			JPrint(txnID)
			panic(err)
		}
	}
	c.log.writeTxnState(tx)
	c.txnPool.Delete(txnID)
	return true
}

func (c *Shard) CommitPostgres(txnID uint32) bool {
	v, ok := c.txnPool.Load(txnID)
	configs.Warn(ok, "the transaction has finished before commit on this node.")
	if !ok {
		return true
	}
	tx := v.(*DBTxn)
	configs.Assert(tx.txnID == txnID, "different transaction running")
	tx.latch.Lock()
	defer tx.latch.Unlock()
	if !tx.TryFinish() {
		return true
	}
	tx.TxnState = txnCommitted
	if tx.TxnState == txnExecution {
		// for a local transaction, there is no prepare phase that persists the redo logs.
		c.log.writeRedoLog4Txn(tx)
	}
	c.log.writeTxnState(tx)
	if tx.isPrepared {
		_, err := tx.sqlTX.Exec(c.ctx, fmt.Sprintf("COMMIT PREPARED 'TXN_%v_%v'", txnID, c.shardID))
		if err != nil {
			panic(err)
		}
		err = tx.sqlTX.Commit(c.ctx)
		//if err != pgx.ErrTxClosed {
		//	// release connection
		//	panic("double commit must be idle")
		//}
	} else {
		if tx.RowCnt > 0 {
			// if some parts of the transaction are processed in OCC locking style.
			if !tx.Validate(c.ctx) {
				tx.sqlTX.Rollback(c.ctx)
				return false
			}
		}
		err := tx.sqlTX.Commit(c.ctx)
		if configs.LockType == configs.RuleBasedLock {
			tx.ruleCC.Finish(err == nil)
		}
		if err != nil {
			return false
			//panic(err)
		}
	}
	c.txnPool.Delete(txnID)
	return true
}

func (c *DBTxn) Validate(ctx context.Context) bool {
	var err error
	// sort write set following primary key to avoid deadlock.
	for i := 0; i < c.RowCnt; i++ {
		x := c.Accesses[i]
		if x.AccessType == TxnWrite {
			for j := i + 1; j < c.RowCnt; j++ {
				y := c.Accesses[j]
				if y.AccessType == TxnWrite && x.Local.PrimaryKey > y.Local.PrimaryKey {
					c.Accesses[i], c.Accesses[j] = c.Accesses[j], c.Accesses[i]
				}
			}
		}
	}

	for i := 0; i < c.RowCnt; i++ {
		v := c.Accesses[i]
		if v.AccessType == TxnWrite {
			_, err = c.sqlTX.Exec(ctx, "select value "+
				"from YCSB_MAIN where key = $1 for update", strconv.FormatUint(uint64(v.Local.PrimaryKey), 10))
			if err != nil {
				//panic(err)
				//if configs.LockType == configs.RuleBasedLock {
				//	c.ruleCC.Report(&TXOpt{Type: UpdateOpt, Key: uint64(v.Local.PrimaryKey)}, false, configs.OCC)
				//}
				return false
			}
		}
	}

	// serialization point here.
	validateOnce := make(map[Key]bool)
	for i := 0; i < c.RowCnt; i++ {
		v := c.Accesses[i]
		var value string
		key := strconv.FormatUint(uint64(v.Local.PrimaryKey), 10)
		if validateOnce[v.Local.PrimaryKey] {
			continue
		}
		validateOnce[v.Local.PrimaryKey] = true
		if v.AccessType == TxnRead {
			err = c.sqlTX.QueryRow(ctx, "select value from YCSB_MAIN where key = $1 for share nowait",
				key).Scan(&value)
			// abort on encountering any dirty data.
			if value != v.Local.Data.String() {
				// warning: implementation specific
				//panic("validate error for update " + key + " " + value + ":" + v.Local.Data.String())
				// according to Silo, the locked record should be treated as dirty and abort soon.
				if configs.LockType == configs.RuleBasedLock {
					c.ruleCC.Report(&TXOpt{Type: ReadOpt, Key: uint64(v.Local.PrimaryKey)}, false, configs.OCC)
				}
				return false
			} else {
				if configs.LockType == configs.RuleBasedLock {
					c.ruleCC.Report(&TXOpt{Type: ReadOpt, Key: uint64(v.Local.PrimaryKey)}, true, configs.OCC)
				}
			}
		}
	}

	c.CommitTS = c.GetTS()

	for i := 0; i < c.RowCnt; i++ {
		v := c.Accesses[i]
		if v.AccessType == TxnWrite {
			_, err = c.sqlTX.Exec(ctx, "update "+
				"YCSB_MAIN set value = $2 where key = $1",
				strconv.FormatUint(uint64(v.Local.PrimaryKey), 10),
				v.Local.Data.String()+":"+strconv.FormatUint(uint64(c.CommitTS), 10))
			// attach the version information to row value.
			if err != nil {
				//panic(err)
				//if configs.LockType == configs.RuleBasedLock {
				//	c.ruleCC.Report(&TXOpt{Type: UpdateOpt, Key: uint64(v.Local.PrimaryKey)}, false, configs.OCC)
				//}
				return false
			}
		}
	}

	return true
}
