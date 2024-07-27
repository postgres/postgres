package participant

import (
	"FC/configs"
	"fmt"
	"sort"
	"strconv"
	"sync"
	"time"
)

// The participant stats function does not work for all ACPs.
// EC and FC may encounter concurrency problem in this stats.

// Info the information of a transaction local branch.
type Info struct {
	Latency time.Duration
	// the time elapsed between a transaction begin to get lock and its lock release.
	ExecutionTime time.Duration
	// the execution time includes tx.Exec, Read/Update etc.
	PrepareTime time.Duration
	// the logging time for "prepared" transaction.
	DecideTime time.Duration
	// the decision persistence time.
	IsAbort bool
	// whether this round of transaction execution is aborted.
	ACP string
	// the transaction local branch is executed with which protocol.
	TID uint64
}

func NewInfo(proto string, txnID uint64) *Info {
	res := &Info{
		Latency:       0,
		ExecutionTime: 0,
		PrepareTime:   0,
		DecideTime:    0,
		IsAbort:       false,
		ACP:           proto,
		TID:           txnID,
	}
	return res
}

// Stat the statistic of transaction processing on a participant.
type Stat struct {
	mu        *sync.Mutex
	nodeID    string
	txnInfos  []*Info
	beginTS   int
	endTS     int
	beginTime time.Time
	endTime   time.Time
}

func NewStat(nodeID string) *Stat {
	res := &Stat{
		txnInfos:  make([]*Info, configs.MaxTID),
		mu:        &sync.Mutex{},
		beginTS:   0,
		endTS:     0,
		beginTime: time.Now(),
		endTime:   time.Now(),
		nodeID:    nodeID,
	}
	return res
}

func (st *Stat) Append(info *Info) {
	st.mu.Lock()
	defer st.mu.Unlock()
	st.endTS++
	_acp := info.ACP
	info.ACP = st.nodeID
	configs.DPrintf(configs.JToString(info))
	info.ACP = _acp
	st.endTime = time.Now()
	st.txnInfos[st.endTS] = info
}

func (st *Stat) Range() {
	st.mu.Lock()
	defer st.mu.Unlock()
	if configs.ProfileStore {
		println(st.beginTS, st.endTS)
		fmt.Printf("Time range [%v  ----  %v]\n", st.beginTime.String(), st.endTime.String())
	}
}

func (st *Stat) Clear() {
	st.mu.Lock()
	defer st.mu.Unlock()
	st.beginTS = st.endTS + 1
	st.beginTime = time.Now()
}

func Min(x int, y int) int {
	if x < y {
		return x
	}
	return y
}

func (st *Stat) Log() {
	st.mu.Lock()
	defer st.mu.Unlock()
	txnCnt, cross, success, crossSuc, contentedAbort := 0, 0, 0, 0, 0
	latencies := make([]int, 0)
	latencySum := 0
	latencyLocal, latencyCross, execCross, execLocal, prepareTime, decideTime, decideLocal := 0, 0, 0, 0, 0, 0, 0
	for i := st.beginTS; i < st.endTS; i++ {
		if st.txnInfos[i] != nil {
			tmp := st.txnInfos[i]
			txnCnt++
			if tmp.IsAbort {
				contentedAbort++
			}
			if tmp.ACP != configs.NoACP {
				cross++
			}
			if tmp.Latency > 0 {
				latencySum += int(tmp.Latency)
				latencies = append(latencies, int(tmp.Latency))
			}
			if !tmp.IsAbort {
				success++
				if tmp.ACP != configs.NoACP {
					execCross += int(tmp.ExecutionTime)
					latencyCross += int(tmp.Latency)
					prepareTime += int(tmp.PrepareTime)
					decideTime += int(tmp.DecideTime)
					crossSuc++
				} else {
					execLocal += int(tmp.ExecutionTime)
					latencyLocal += int(tmp.Latency)
				}
			}
		}
	}
	msg := "node:" + st.nodeID + ";"
	msg += "txn_cnt:" + strconv.Itoa(txnCnt/configs.RunParticipantProfilerInterval) + ";"
	msg += "dis_txn_cnt:" + strconv.Itoa(cross/configs.RunParticipantProfilerInterval) + ";"
	msg += "success_txn:" + strconv.Itoa(success/configs.RunParticipantProfilerInterval) + ";"
	msg += "success_dis_txn:" + strconv.Itoa(crossSuc/configs.RunParticipantProfilerInterval) + ";"
	msg += "cc_abort:" + strconv.Itoa(contentedAbort/configs.RunParticipantProfilerInterval) + ";"
	sort.Ints(latencies)
	if len(latencies) > 0 {
		i := Min((len(latencies)*99+99)/100, len(latencies)-1)
		msg += "p99_latency:" + time.Duration(time.Duration(latencies[i]).Nanoseconds()).String() + ";"
		i = Min((len(latencies)*9+9)/10, len(latencies)-1)
		msg += "p90_latency:" + time.Duration(time.Duration(latencies[i]).Nanoseconds()).String() + ";"
		i = Min((len(latencies)+1)/2, len(latencies)-1)
		msg += "p50_latency:" + time.Duration(time.Duration(latencies[i]).Nanoseconds()).String() + ";"
		msg += "ave_latency:" + time.Duration(time.Duration(float64(latencySum)/float64(len(latencies))).Nanoseconds()).String() + ";"
	} else {
		msg += "p99_latency:nil;"
		msg += "p90_latency:nil;"
		msg += "p50_latency:nil;"
		msg += "ave_latency:nil;"
	}

	if crossSuc == success {
		msg += "local_tx_blk_window:nil;"
	} else {
		msg += "local_tx_blk_window:" + time.Duration(int64(float64(latencyLocal)/float64(success-crossSuc))).String() + ";"
	}

	if crossSuc == 0 {
		msg += "ex:" + time.Duration(int64(float64(execLocal)/float64(success-crossSuc))).String() + ","
		msg += "de:" + time.Duration(int64(float64(decideLocal)/float64(success-crossSuc))).String() + ","
	} else {
		msg += "cross_tx_blk_window:" + time.Duration(int64(float64(latencyCross)/float64(crossSuc))).String() + "{"
		msg += "ex:" + time.Duration(int64(float64(execCross)/float64(crossSuc))).String() + ","
		msg += "pr:" + time.Duration(int64(float64(prepareTime)/float64(crossSuc))).String() + ","
		msg += "bl:" + time.Duration(int64(float64(latencyCross-prepareTime-execCross-decideTime)/float64(crossSuc))).String() + ","
		msg += "de:" + time.Duration(int64(float64(decideTime)/float64(crossSuc))).String() + "};"
	}
	fmt.Println(msg)
}
