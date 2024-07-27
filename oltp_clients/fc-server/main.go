package main

import (
	"FC/benchmark"
	"FC/configs"
	"FC/network/participant"
	"flag"
	"fmt"
	"github.com/jackc/pgx/v4"
	"io"
	"log"
	"os"
	"runtime"
	"runtime/pprof"
	"runtime/trace"
	"time"
)

var (
	part          string
	protocol      string
	dist          string
	numPart       int
	l             int
	con           int
	cf            int
	nf            int
	down          int
	minLevel      int
	cross         int
	wh            int
	r             float64
	partPerTxn    int
	bench         string
	local         bool
	debug         bool
	elapsed       bool
	sk            float64
	addr          string
	tb            int
	rw            float64
	replica       bool
	fastVote      bool
	cpuProfile    string
	memProfile    string
	dVar          float64
	injectCCDelay float64
	iso           string
	lock          string
)

func usage() {
	flag.PrintDefaults()
}

// DefaultIsolationLevel

func init() {
	flag.IntVar(&numPart, "part", 1, "the number of participants")
	flag.IntVar(&l, "len", 16, "the transaction length")
	flag.StringVar(&addr, "addr", "127.0.0.1:5001", "the address for this node")
	flag.StringVar(&part, "node", "coordinator", "the node to start")
	flag.StringVar(&cpuProfile, "cpu_prof", "", "write cpu profiling")
	flag.StringVar(&memProfile, "mem_prof", "", "write memory profiling")
	flag.Float64Var(&sk, "skew", 0.5, "the skew factor for ycsb zipf")
	flag.IntVar(&cross, "cross", 0, "the cross shard transaction percentage (%).")
	flag.IntVar(&partPerTxn, "txn_part", 2, "the number of shard each transaction accesses.")
	flag.IntVar(&con, "c", 8, "the number of clients")
	flag.Float64Var(&rw, "rw", 0.5, "the read percentage")
	flag.Float64Var(&injectCCDelay, "delay", 0, "the ms delay injected into the slow stores")
	flag.StringVar(&bench, "bench", "ycsb", "the benchmark used for the test")
	flag.StringVar(&iso, "iso", "", "the isolation level of PG")
	flag.StringVar(&lock, "lock", "none", "the lock strategy of PG")
	//flag.StringVar(&store, "store", configs.BenchmarkStorage, "the storage benchmark")
	flag.StringVar(&protocol, "p", "fc", "the protocol used for this test")
	flag.IntVar(&wh, "wh", 64, "The number of warehouse.")
	flag.IntVar(&down, "d", 4, "The heuristic method used: x for fixed timeout, 0 for RL.")
	flag.Float64Var(&r, "r", 2.0, "The network parameter r.")
	flag.StringVar(&dist, "dis", configs.Exponential, "The failure distribution (exp, poisson, or normal).")
	flag.BoolVar(&local, "local", false, "run local test")
	flag.BoolVar(&elapsed, "elapsed", false, "how time line experiment, sample every 10ms")
	flag.BoolVar(&debug, "debug", false, "log debug info into debug file")
	flag.BoolVar(&replica, "replica", false, "enable replicated shards")
	flag.BoolVar(&fastVote, "fv", false, "take a short timeout in the get vote phase to avoid blocking resources for too long")
	flag.IntVar(&cf, "cf", -1, "the expected time for a crash failure to happen, -1 for no failure.")
	flag.IntVar(&nf, "nf", -1, "the expected time for a network failure to happen, -1 for no failure.")
	flag.Float64Var(&dVar, "dvar", 0, "the delay variance for network connections.")
	flag.IntVar(&minLevel, "ml", 0, "The smallest level can be used.")
	flag.IntVar(&tb, "tb", 10000, "The YCSB table size per shard (tb * numPart = total record number).")

	flag.Usage = usage
}

func main() {
	flag.Parse()
	if debug {
		f, err := os.OpenFile(fmt.Sprintf("logs/logfiles_%v.log", time.Now().String()), os.O_RDWR|os.O_CREATE, 0666)
		defer f.Close()
		if err != nil {
			log.Fatalf("error opening file: %v", err)
		}
		log.SetOutput(io.Writer(f))
	}

	if configs.TraceFile {
		traceFile, err := os.OpenFile(fmt.Sprintf("logs/trace_%v.log", time.Now().String()), os.O_RDWR|os.O_CREATE, 0666)
		if err != nil {
			log.Fatalf("error opening file: %v", err)
		}

		defer traceFile.Close()
		err = trace.Start(traceFile)
		if err != nil {
			panic(err)
		}

		defer trace.Stop()
	}
	if cpuProfile != "" {
		f, err := os.Create(cpuProfile)
		if err != nil {
			log.Fatal("could not create CPU profile: ", err)
		}
		defer f.Close() // error handling omitted for example
		if err := pprof.StartCPUProfile(f); err != nil {
			log.Fatal("could not start CPU profile: ", err)
		}
		defer pprof.StopCPUProfile()
	}
	if iso == "s" {
		configs.DefaultIsolationLevel = pgx.Serializable
	} else if iso == "si" {
		configs.DefaultIsolationLevel = pgx.RepeatableRead
	} else if iso == "rc" {
		configs.DefaultIsolationLevel = pgx.ReadCommitted
	} else {
		configs.DefaultIsolationLevel = pgx.ReadUncommitted
	}
	if lock == "2plnw" {
		configs.LockType = configs.NoWait2PL
	} else if lock == "2pl" {
		configs.LockType = configs.Native2PL
	} else if lock == "2plwd" {
		configs.LockType = configs.WaitDie2PL
	} else if lock == "none" {
		configs.LockType = configs.NoLock
	} else if lock == "occ" {
		configs.LockType = configs.OCC
	} else if lock == "rule" {
		configs.LockType = configs.RuleBasedLock
	} else {
		configs.LockType = configs.LearnedCC
	}
	configs.InjectDelay = time.Duration(injectCCDelay * float64(time.Millisecond))
	configs.DelayStdDev = time.Duration(dVar * float64(configs.ExpBaseDelay)) // this is only used for 10ms delay experiments
	// in the distributed data center. The standard variance is set to dVar * 10ms.
	//configs.JPrint(configs.DelayStdDev)
	configs.Distribution = dist
	configs.NetWorkDelayParameter = r
	configs.NumberOfShards = numPart
	configs.ClientRoutineNumber = con
	if down > 0 {
		configs.SetDown(down)
	} else {
		configs.SetDown(0)
	}
	//configs.StorageType = store
	configs.EnableReplication = replica
	configs.EnableQuickPreWriteAbort = fastVote
	configs.CrossShardTXNPercentage = cross
	configs.ShardsPerTransaction = partPerTxn
	configs.YCSBDataSkewness = sk
	configs.TransactionLength = l
	//configs.ShowDebugInfo = debug
	configs.ShowWarnings = debug
	configs.ShowTestInfo = debug
	//configs.ShowRobustnessLevelChanges = debug
	configs.TimeElapsedTest = elapsed
	configs.NumberOfWareHousePerShards = wh
	configs.Benchmark = bench

	configs.ReadPercentage = rw
	configs.SetFailureInjection(cf, nf)
	configs.SetMinLevel(minLevel)
	if local {
		configs.SetLocal()
	}
	configs.SetProtocol(protocol)
	if configs.SelectedACP == configs.GPAC {
		configs.EnableReplication = true
	}

	if part == "p" {
		if bench == "ycsb" {
			configs.NumberOfRecordsPerShard = tb
		}
		participant.Main(addr)
	} else if part == "c" {
		if bench == "ycsb" {
			configs.NumberOfRecordsPerShard = tb
			benchmark.TestYCSB(protocol, addr)
		} else if bench == "tpc" {
			configs.NumberOfRecordsPerShard = configs.TPCRecordPerShard
			benchmark.TestTPC(protocol, addr)
		}
	} else {
		panic("invalid parameter for part, 'p' for participant or 'c' for coordinator")
	}
	if memProfile != "" {
		f, err := os.Create(memProfile)
		if err != nil {
			log.Fatal("could not create memory profile: ", err)
		}
		defer f.Close() // error handling omitted for example
		runtime.GC()    // get up-to-date statistics
		if err := pprof.WriteHeapProfile(f); err != nil {
			log.Fatal("could not write memory profile: ", err)
		}
	}
}
