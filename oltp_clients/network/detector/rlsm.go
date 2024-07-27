package detector

import (
	"FC/configs"
	"fmt"
	"github.com/viney-shih/go-lock"
)

type Level int

const (
	NoCFNoNF   Level = 1
	CFNoNF     Level = 2
	CFNF       Level = 3
	NoCFNF     Level = 4
	EasyCommit Level = 5 // the mark is used for easy commit.
)

// LevelStateMachine is the thread safe Level machine maintained on the DBMS, each shard is assigned with one.
type LevelStateMachine struct {
	Id        int
	latch     lock.RWMutex // mutex for heurstic method, to access the model sequentially.
	Level     Level        // the current Level of shards robustness
	H         int
	DownClock int
	from      *LevelStateManager
}

func (s *LevelStateMachine) String() string {
	return fmt.Sprintf("[LSM](id:%v, level:%v, h:%v, downlock:%v)", s.Id, s.Level, s.H, s.DownClock)
}

func NewLSM(s *LevelStateManager) *LevelStateMachine {
	Fixed = make([]SimpleLearner, 0)
	for i := 0; i < configs.NumberOfShards; i++ {
		Fixed = append(Fixed, SimpleLearner{level: 1, cnt: configs.DetectorInitWaitCnt, react: -1})
	}
	for i := 0; i < configs.NumberOfShards; i++ {
		Fixed = append(Fixed, SimpleLearner{level: 1, cnt: 1025 - configs.DetectorInitWaitCnt, react: -1})
	}
	return &LevelStateMachine{
		latch:     lock.NewCASMutex(),
		Level:     NoCFNoNF,
		H:         0,
		DownClock: 0,
		from:      s,
	}
}

func (c *LevelStateMachine) GetLevel() Level {
	return c.Level
}

func (c *LevelStateMachine) Down() {
	c.Level = NoCFNoNF
}

// Next thread safely upward transform the state machine with the results handled.
func (c *LevelStateMachine) Next(CrashF bool, NetF bool, comLevel Level, id string) error {
	if c.Level <= comLevel {
		// the Level has been updated by another client, current result is no longer valid.
		if c.Level == NoCFNoNF {
			if NetF {
				c.Level = CFNoNF
				configs.LPrintf("upppppp!!!!!" + id)
			} else if CrashF {
				c.Level = CFNoNF
				configs.LPrintf("upppppp!!!!!" + id)
			}
		} else if c.Level == CFNoNF {
			if NetF {
				c.Level = CFNF
				configs.LPrintf("upppppp!!!!! to nF" + id)
			}
		}
	}

	// For downward transitions. operations that are too close are abandoned by latch
	c.DownClock++
	if c.DownClock >= configs.DetectorDownBatchSize {
		ok := c.latch.TryLockWithTimeout(AccessInterval)
		level := c.Level
		if ok && configs.FCMinRobustnessLevel >= 0 {
			c.Trans(level, NetF || CrashF, id)
			c.latch.Unlock()
		}
		c.DownClock = 0
	}
	return nil
}
