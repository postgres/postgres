package participant

import (
	"FC/configs"
	"context"
	"github.com/goccy/go-json"
	"math"
	"math/rand"
	"os"
	"sort"
	"strconv"
	"sync"
	"time"
)

// Context records the statement context for Manager nodes.
type Context struct {
	mu           *sync.Mutex
	ctx          context.Context
	coordinator  string
	participants []string
	address      string
	wLatch       *sync.Mutex
	w            map[string]time.Duration // w(i) =  latency(coordinator,participant_i) + latency(participant_i, cur)
	cancel       context.CancelFunc
	queueLatch   *sync.Mutex
	msgQueue     [][]byte
	stats        *Stat

	Manager *Manager // the participant manager

	done chan bool
	conn *Comm
}

var conLock = sync.Mutex{}
var config map[string]interface{}

func initData(stmt *Context, service string) {
	loadConfig(stmt, &config)
	stmt.msgQueue = make([][]byte, 0)
	configs.TPrintf("Load config finished")
	stmt.mu = &sync.Mutex{}
	stmt.wLatch = &sync.Mutex{}
	stmt.stats = NewStat(service)
	stmt.queueLatch = &sync.Mutex{}
	stmt.address = service
	storageSize := configs.NumberOfRecordsPerShard
	stmt.Manager = NewParticipantManager(stmt, storageSize)
}

func loadConfig(stmt *Context, config *map[string]interface{}) {
	conLock.Lock()
	defer conLock.Unlock()
	/* Read the config file and store it in 'config' variable */
	raw, err := os.ReadFile(configs.ConfigFileLocation)
	if err != nil {
		raw, err = os.ReadFile("." + configs.ConfigFileLocation)
	}
	configs.CheckError(err)

	err = json.Unmarshal(raw, &config)
	tmp, _ := ((*config)["participants"]).(map[string]interface{})
	stmt.participants = make([]string, 0)
	stmt.w = make(map[string]time.Duration)
	for i, p := range tmp {
		tp, err := strconv.Atoi(i)
		configs.CheckError(err)
		if tp <= configs.NumberOfShards {
			stmt.w[p.(string)] = 2 * configs.ExpBaseDelay
			stmt.participants = append(stmt.participants, p.(string))
		}
	}
	sort.Strings(stmt.participants)
	if len(configs.OuAddress) == 0 {
		configs.OuAddress = stmt.participants
	}
	tmp, _ = ((*config)["coordinators"]).(map[string]interface{})
	for _, p := range tmp {
		stmt.coordinator = p.(string)
	}
	stmt.done = make(chan bool, 1)
	configs.CheckError(err)
}

// Close the running participant process.
func (ctx *Context) Close() {
	configs.TPrintf("Close called!!! at " + ctx.address)
	ctx.done <- true
	ctx.cancel()
	ctx.conn.Stop()
}

func begin(stmt *Context, ch chan bool, service string) {
	configs.TPrintf("Initializing -- ")
	initData(stmt, service)
	configs.DPrintf(service)
	stmt.ctx, stmt.cancel = context.WithCancel(context.Background())
	stmt.conn = NewConns(stmt, service)

	configs.DPrintf("build finished for " + service)

	if (!configs.LocalTest || stmt.address == configs.OuAddress[0]) && configs.ServerAutoCrashEnabled {
		stmt.injectCrashFailures()
	}
	if configs.NetworkDisruptEnabled { // inject network disrupts to all the participants.
		stmt.injectNetworkDisrupts()
	}
	ch <- true
	stmt.Run()
}

// Main the main function for a participant process.
func Main(addr string) {
	stmt := &Context{}
	ch := make(chan bool)
	go func() {
		<-ch
		if configs.Benchmark == "ycsb" {
			stmt.YCSBInit()
		} else {
			stmt.TPCInit()
		}
	}()
	begin(stmt, ch, addr)
}

func (ctx *Context) injectCrashFailures() {
	if configs.ExpectedCrashTime == 0 {
		go func() {
			configs.Assert(configs.DelayStaticPreHeat < configs.WarmUpTime, "not enough warmup time")
			time.Sleep(configs.DelayStaticPreHeat)
			ctx.Manager.Break()
		}()
	} else {
		go func() {
			configs.Assert(configs.DelayStaticPreHeat < configs.WarmUpTime, "not enough warmup time")
			time.Sleep(configs.DelayStaticPreHeat)
			for {
				var nextFailureTime time.Duration
				if configs.Distribution == configs.Normal {
					nextFailureTime = time.Duration(math.Abs(rand.NormFloat64() * float64(configs.ExpectedCrashTime)))
				} else if configs.Distribution == configs.Exponential {
					nextFailureTime = time.Duration(math.Abs(rand.ExpFloat64() * float64(configs.ExpectedCrashTime)))
				} else if configs.Distribution == configs.Plain {
					nextFailureTime = configs.ExpectedCrashTime
				} else {
					panic("invalid distribution")
				}
				select {
				case <-ctx.ctx.Done():
					return
				case <-time.After(nextFailureTime):
					ctx.Manager.Break()
					time.Sleep(configs.CrashPeriod)
					ctx.Manager.Recover()
				}
			}
		}()
	}
}

func (ctx *Context) injectNetworkDisrupts() {
	// for test, to simulate the jerky environments.
	if configs.ExpectedDelayTime == 0 {
		go func() {
			configs.Assert(configs.DelayStaticPreHeat < configs.WarmUpTime, "not enough warmup time")
			time.Sleep(configs.DelayStaticPreHeat)
			ctx.Manager.NetBreak()
		}()
	} else {
		go func() {
			configs.Assert(configs.DelayStaticPreHeat < configs.WarmUpTime, "not enough warmup time")
			time.Sleep(configs.DelayStaticPreHeat)
			for {
				var nextFailureTime time.Duration
				if configs.Distribution == configs.Normal {
					nextFailureTime = time.Duration(math.Abs(rand.NormFloat64() * float64(configs.ExpectedDelayTime)))
				} else if configs.Distribution == configs.Exponential {
					nextFailureTime = time.Duration(math.Abs(rand.ExpFloat64() * float64(configs.ExpectedDelayTime)))
				} else if configs.Distribution == configs.Plain {
					nextFailureTime = configs.ExpectedDelayTime
				} else {
					panic("invalid distribution")
				}
				select {
				case <-ctx.ctx.Done():
					return
				case <-time.After(nextFailureTime):
					ctx.Manager.NetBreak()
					time.Sleep(configs.DelayPeriod)
					ctx.Manager.NetRecover()
				}
			}
		}()
	}
}

func (ctx *Context) UpdateNetworkDelay(fromParticipant string, delay time.Duration) {
	ctx.wLatch.Lock()
	defer ctx.wLatch.Unlock()
	old, ok := ctx.w[fromParticipant]
	configs.Assert(ok, "the network timeout window is not initialized")
	if old == configs.CrashFailureTimeout {
		ctx.w[fromParticipant] = delay
	} else {
		// adjust the network timeout window.
		ctx.w[fromParticipant] = time.Duration(0.99*float64(old) + 0.01*float64(delay))
	}
}

func (ctx *Context) GetNetworkTimeOut(part []string) time.Duration {
	// max{ dis(C,j) + dis(j, aim) | j in ParticipantAddresses}
	ctx.wLatch.Lock()
	defer ctx.wLatch.Unlock()
	res := time.Duration(0)
	for _, p := range part {
		if res < ctx.w[p] {
			res = ctx.w[p]
		}
	}
	return time.Duration(float64(res) * configs.NetWorkDelayParameter)
}

func (ctx *Context) Run() {
	ctx.stats.Clear()
	if configs.ProfileStore {
		go ctx.Profiler()
	}
	ctx.conn.Run()
}

func (ctx *Context) GetAddr() string {
	return ctx.address
}

func (ctx *Context) Profiler() {
	for !ctx.Manager.txnComing {
		time.Sleep(time.Second)
	} // block until all nodes have finished initialization
	ctx.stats.Clear()
	time.Sleep(configs.ParticipantWarmUpTime)
	ctx.stats.Clear()
	select {
	case <-time.After(configs.RunParticipantProfilerInterval * time.Second):
		ctx.stats.Log()
	case <-ctx.ctx.Done():
		ctx.stats.Log()
		panic("too long RunParticipantProfilerInterval")
	}
	//ctx.stats.Range()
	ctx.stats.Clear()
}
