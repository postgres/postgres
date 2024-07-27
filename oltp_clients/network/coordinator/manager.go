package coordinator

import (
	"FC/configs"
	"FC/network"
	"FC/network/detector"
	"FC/storage"
	"FC/utils"
	"strconv"
	"sync"
	"time"
)

const (
	None          = 0
	PreRead       = 1
	PreWrite      = 2
	Committed     = 3
	TransitCommit = 4
	Aborted       = 5
	AgCommitted   = 6
	Abnormal      = 7
	TransitAbort  = 8
	Propose       = 9
)

// Manager serves as a manager of transactions for the coordinator.
type Manager struct {
	stmt         *Context
	Lsm          *detector.LevelStateManager
	Participants []string
	Replicas     map[string][]string
	TxnPool      *sync.Map
	logs         *LogManager
}

func NewManager(stmt *Context) *Manager {
	res := &Manager{
		stmt:         stmt,
		Replicas:     stmt.replicas,
		Lsm:          detector.NewLSMManger(stmt.participants),
		Participants: stmt.participants,
		TxnPool:      &sync.Map{},
		logs:         NewLogManager(stmt.coordinatorID),
	}
	return res
}

func (c *Manager) TrySubmit(txn *TX, protocol string, info *utils.Info) bool {
	defer configs.TimeLoad(time.Now(), "Submit transaction", txn.TxnID, &info.Latency)
	N := len(txn.Participants)
	info.IncorrectAssumption = false
	if N == 1 {
		configs.TPrintf("TXN" + strconv.FormatUint(txn.TxnID, 10) + ": the transaction get submitted with single shard")
		// do not use atomic commit Protocol for single sharded transactions.
		info.IsCommit = c.SingleSubmit(txn, info)
		return info.IsCommit
	}
	switch protocol {
	case configs.FC:
		info.IsCommit = c.FCSubmit(txn, info)
	case configs.FCff:
		info.IsCommit = c.FCffSubmit(txn, info, 0)
	case configs.FCcf:
		info.IsCommit = c.FCcfSubmit(txn, info)
	case configs.TwoPC:
		info.IsCommit = c.TwoPCSubmit(txn, info)
	case configs.ThreePC:
		info.IsCommit = c.ThreePCSubmit(txn, info)
	case configs.PAC:
		info.IsCommit = c.PACSubmit(txn, info)
	case configs.EasyCommit:
		info.IsCommit = c.EasySubmit(txn, info)
	case configs.GPAC:
		info.IsCommit = c.GPACSubmit(txn, info)
	case configs.LearnedC:
		info.IsCommit = c.LearnedSubmit(txn, info)
	default:
		configs.Assert(false, "Incorrect Protocol "+protocol)
		return false
	}
	if configs.SimulateClientSideDelay && (protocol != configs.FCff ||
		info.Failure || (info.Level > 1 && info.IsCommit)) {
		// to simulate 10ms delay between coordinator and client.
		// when Protocol = "fc" and we execute with fast path,
		// the results are returned directly from the participants to the coordinator, thus we do not need to wait for it.
		time.Sleep(10 * time.Millisecond)
	}
	return info.IsCommit
}

// SubmitTxn submit a transaction with network Protocol.
func (c *Manager) SubmitTxn(txn *TX, protocol string, info *utils.Info) bool {
	N := len(txn.Participants)
	if info == nil {
		info = utils.NewInfo(N)
	} else {
		info.NumPart = N
	}
	defer configs.TimeLoad(time.Now(), "Submit transaction", txn.TxnID, &info.Latency)
	txn.Optimize()
	configs.TPrintf("TXN" + strconv.FormatUint(txn.TxnID, 10) + ": Begin!!!!")
	res := c.TrySubmit(txn, protocol, info)
	retryTime := configs.InitPenalty4Abort
	info.RetryCount = 1
	oldTID := txn.TxnID
	// Transactions aborted due to contention shall get retried.
	// https://github.com/googleapis/google-cloud-java/issues/1230
	for !res && !info.Failure && info.RetryCount < configs.MaxRetry {
		// We follow An Evaluation of Distributed Concurrency Control to adopt an exponential penalty starting at 10ms.
		// https://github.com/xxx/deneva/blob/master/system/abort_queue.cpp
		if info.IncorrectAssumption {
			info.IncorrectAssumptionCnt++
		} else {
			info.CCRetry++
		}
		retryTime *= 2
		time.Sleep(retryTime)
		txn.TxnID = utils.GetTxnID()
		configs.TPrintf("TXN" + strconv.FormatUint(txn.TxnID, 10) + ": retrying transaction for TXN:" + strconv.FormatUint(oldTID, 10) + " next " + retryTime.String())
		oldTID = txn.TxnID
		res = c.TrySubmit(txn, protocol, info)
		info.RetryCount++
	}
	if info.IncorrectAssumption {
		info.IncorrectAssumptionCnt++
	}
	return res
}

func (c *Manager) readValue(txn *TX) (map[string]*storage.RowData, bool) {
	handler := c.createIfNotExistTxnHandler(txn.TxnID, configs.SelectedACP, len(txn.Participants))
	handler.transit(None, PreRead)
	handler.clearMsgPool()
	handler.VoterNumber = len(txn.Participants)
	branches := make(map[string]*network.CoordinatorGossip)
	for _, v := range txn.Participants {
		branches[v] = network.NewTXPack(txn.TxnID, v, configs.SelectedACP, txn.Participants)
	}
	for _, v := range txn.OptList {
		switch v.Type {
		case storage.ReadOpt:
			branches[v.Shard].AppendRead(v.Table, v.Shard, v.Key)
		default:
			configs.Assert(false, "no write should be sent to the pre-read")
		}
	}
	for i, op := range branches {
		txn.sendPreRead(i, op)
	}
	select {
	case <-time.After(configs.CrashFailureTimeout):
		return nil, false
	case <-c.stmt.ctx.Done():
		return nil, false
	case <-handler.finish:
		if handler.allACKCollected() {
			return handler.extractResult(), true
		} else {
			return nil, false
		}
	}
}

// PreRead pre-read API, read the value from remote database first.
func (c *Manager) PreRead(txn *TX) (map[string]*storage.RowData, bool) {
	panic("the pre read shall not be called in local benchmarking")
	//if configs.StoredProcedure {
	//	return make(map[string]*storage.RowData), true
	//}
	//defer c.clearTxnHandler(txn.TxnID)
	//configs.Assert(!configs.StoredProcedure, "store procedure shall not call pre-read")
	//txn.Optimize()
	//res, ok := c.readValue(txn)
	//if !configs.StoredProcedure {
	//	if ok {
	//		return res, ok
	//	} else {
	//		c.DecideAsync(txn, false, nil)
	//		return nil, false
	//	}
	//} else {
	//	return res, ok
	//}
}

// PDServer is used to manage critical meta-data for the coordinator.
type PDServer struct {
	mu           *sync.Mutex
	state        *detector.LevelStateManager // the state machine Manager
	shardMapper  map[int]int                 // the map from an overall address to the shard id.
	offsetMapper map[int]int                 // the map from an overall address to the offset in the shard.
}

// Register a location in PD.
func (c *PDServer) Register(addr int, shard int, offset int) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if val, ok := c.shardMapper[addr]; ok && (val != shard || c.offsetMapper[addr] != offset) {
		return false
	}
	c.shardMapper[addr] = shard
	c.offsetMapper[addr] = offset
	return true
}
