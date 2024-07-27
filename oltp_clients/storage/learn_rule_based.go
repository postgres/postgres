package storage

import (
	"FC/configs"
)

// At the beginning: if very highly skew +  --> use SSI to maximize concurrency.

// Observation:
//	If contention (historical + current txn) is high AND much write operations: SSI.
//	If contention (historical + current txn) is low OR much read operations: OCC.
//  if contention is low AND much read operations: 2PL is sometimes better ???
//  If read heavy + low contention: 2PL?
//  If the transaction is read only (historical + current txn) --> 2PL.

// why SSI better than others in high contention + write heavy.
// 1. OCC --> likely to be aborted. (we need a factor that capture how likely a read to be changed)
// 2. 2PL --> contention and deadlock. (SSI + MVCC enables multi-version, and thus allows for more concurrency)

// why OCC better than others in low skewness?
// 1. SSI, ssi verification is more costly than OCC.
// 2. 2PL locking overhead dominates? The performance seems unstable.

// why 2PL better than others in read-only?
// 1. the 2PL only do read latch, no update needed.

// For an incoming read operation.
//

const eps = 1e-6
const LearningRate = 0.1

type RBGlobalStatus struct {
	readProb float64
	sucProb  float64
	failProb []float64
}

func NewRBGlobalStatus() *RBGlobalStatus {
	res := &RBGlobalStatus{
		readProb: 0,
		sucProb:  1,
		failProb: make([]float64, configs.NumberOfRecordsPerShard),
	}
	//for i := 0; i < configs.NumberOfRecordsPerShard; i++ {
	//	res.failProb[i] = 0
	//}
	return res
}

type RBTXStatus struct {
	global   *RBGlobalStatus
	sucProb  float64
	readCnt  int
	writeCnt int
	isSSI    bool
}

func NewRBTXStatus(g *RBGlobalStatus, opt []TXOpt) *RBTXStatus {
	return &RBTXStatus{
		global:   g,
		sucProb:  1,
		readCnt:  0,
		writeCnt: 0,
		isSSI:    g.IsSSI(opt),
	}
}

func (s *RBGlobalStatus) IsSSI(opt []TXOpt) bool {
	//if s.readProb > 0.99 {
	//	return false
	//}
	//JPrint(configs.ClientRoutineNumber)
	if configs.ClientRoutineNumber == 1 {
		return true
	}
	if opt == nil {
		panic("interactive not supported yet")
		return false
	}
	sucProb := 1.0
	for _, v := range opt {
		sucProb = sucProb * (1 - s.failOnKey(v.Key))
		if sucProb < configs.SSISwitchThr {
			//println(s.failOnKey(v.Key))
			return true
		}
	}
	return false
}

func (s *RBGlobalStatus) failOnKey(key uint64) float64 {
	return s.failProb[key]
}

func (s *RBGlobalStatus) ReportFin(readRatio float64, sucProb float64) {
	s.sucProb = (1-LearningRate)*s.sucProb + LearningRate*sucProb
	s.readProb = (1-LearningRate)*s.readProb + LearningRate*readRatio
}

func (s *RBGlobalStatus) ReportLockSuc(key uint64, suc bool, rate float64) {
	s.failProb[key] = (1 - rate) * s.failProb[key]
	if !suc {
		s.failProb[key] += rate
	}
}

func (s *RBTXStatus) IsOptimistic(opt *TXOpt) bool {
	return false
	//if 1-s.global.readProb < eps && s.writeCnt == 0 {
	//	println("case 1")
	//	// for a read-only transaction, select 2PL.
	//	return false
	//}
	//if s.sucProb*(1-s.global.failOnKey(opt.Key)) > 0.2 {
	//	s.sucProb = s.sucProb * (1 - s.global.failOnKey(opt.Key))
	//	return true
	//} else {
	//	return false
	//}
}

func (s *RBTXStatus) GetNext(opt *TXOpt) string {
	if s.isSSI {
		return configs.NoLock
	} else if s.IsOptimistic(opt) {
		return configs.OCC
	} else {
		return configs.Native2PL
	}
}

func (s *RBTXStatus) Report(opt *TXOpt, suc bool, lock string) {
	if opt.Type == ReadOpt {
		s.readCnt++
	} else {
		s.writeCnt++
	}
	//if lock != configs.Lock {
	if s.isSSI {
		s.global.ReportLockSuc(opt.Key, suc, 0.001)
	} else {
		s.global.ReportLockSuc(opt.Key, suc, 0.1)
	}
	//}
}

func (s *RBTXStatus) Finish(suc bool) {
	if suc {
		s.global.ReportFin(float64(s.readCnt)/float64(s.readCnt+s.writeCnt), 1)
	} else {
		s.global.ReportFin(float64(s.readCnt)/float64(s.readCnt+s.writeCnt), 0)
	}
}
