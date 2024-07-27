package coordinator

import (
	"FC/configs"
	"FC/network/detector"
	"context"
	"github.com/goccy/go-json"
	"io/ioutil"
	"os"
	"sort"
	"strconv"
	"sync"
	"time"
)

// Context records the statement context for a coordinator node.
type Context struct {
	// string address -> node.
	Manager       *Manager
	coordinatorID string
	participants  []string
	replicas      map[string][]string
	conn          *Commu
	wLatch        *sync.Mutex
	w             map[string]time.Duration // w(i) =  reactTime in the pre-write/propose phase.
	ctx           context.Context
	cancel        context.CancelFunc
}

var conLock = sync.Mutex{}
var config map[string]interface{}

// [] [address] [storageSize]
func initData(stmt *Context, Args []string) {
	loadConfig(stmt, &config)
	stmt.coordinatorID = Args[2]
	stmt.wLatch = &sync.Mutex{}
	stmt.Manager = NewManager(stmt)
}

func loadConfig(stmt *Context, config *map[string]interface{}) {
	conLock.Lock()
	defer conLock.Unlock()
	/* Read the config file and store it in 'config' variable */
	raw, err := ioutil.ReadFile(configs.ConfigFileLocation)
	if err != nil {
		raw, err = ioutil.ReadFile("." + configs.ConfigFileLocation)
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
			stmt.w[configs.FCResults+p.(string)] = 4 * configs.ExpBaseDelay
			stmt.w[configs.PreWriteACK+p.(string)] = 4 * configs.ExpBaseDelay
			stmt.participants = append(stmt.participants, p.(string))
		}
	}
	sort.Strings(stmt.participants)
	if len(configs.OuAddress) == 0 {
		configs.OuAddress = stmt.participants
	}
	tmp, _ = ((*config)["coordinators"]).(map[string]interface{})
	for _, p := range tmp {
		stmt.coordinatorID = p.(string)
	}
	stmt.replicas = make(map[string][]string)
	if configs.EnableReplication {
		// cycle replication: i replicated to i+1, i+2, i+R mod N.
		for i := 0; i < configs.NumberOfShards; i++ {
			for j := 0; j < configs.NumberOfReplicas; j++ {
				stmt.replicas[stmt.participants[i]] = append(stmt.replicas[stmt.participants[i]],
					stmt.participants[(i+j)%configs.NumberOfShards])
			}
		}
	}
	//configs.JPrint(stmt.replicas)
	//configs.JPrint(configs.EnableReplication)
	configs.CheckError(err)
}

func (c *Context) Close() {
	detector.Stop()
	c.cancel()
	c.conn.Close()
}

func begin(stmt *Context, Args []string, ch chan bool) {
	initData(stmt, Args)

	service := Args[2]
	configs.DPrintf(service)
	stmt.conn = NewConns(stmt, service)
	stmt.ctx, stmt.cancel = context.WithCancel(context.Background())
	ch <- true
	stmt.conn.Run()
}

func Main() {
	stmt := &Context{}
	ch := make(chan bool)
	begin(stmt, os.Args, ch)
}

func (ctx *Context) UpdateNetworkDelay(fromParticipant string, delay time.Duration, mark string) {
	ctx.wLatch.Lock()
	defer ctx.wLatch.Unlock()
	x := mark + fromParticipant
	old, ok := ctx.w[x]
	configs.Assert(ok, "the network timeout window is not initialized")
	if old == configs.CrashFailureTimeout {
		ctx.w[x] = delay
	} else {
		// adjust the network timeout window.
		ctx.w[x] = time.Duration(0.99*float64(old) + 0.01*float64(delay))
	}
}

func (ctx *Context) GetNetworkTimeOut(part []string, mark string) time.Duration {
	ctx.wLatch.Lock()
	defer ctx.wLatch.Unlock()
	res := time.Duration(0)
	for _, p := range part {
		x := mark + p
		if res < ctx.w[x] {
			res = ctx.w[x]
		}
	}
	return time.Duration(float64(res) * configs.NetWorkDelayParameter)
}
