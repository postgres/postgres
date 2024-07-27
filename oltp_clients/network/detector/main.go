package detector

import (
	"FC/configs"
	"sync"
)

type Learner interface {
	Send(level int, cid string)
	Action(cid string) int
}

var lockStat = &sync.Mutex{}
var CommittedTxn = 0

func Add_th() {
	lockStat.Lock()
	defer lockStat.Unlock()
	CommittedTxn++
}

func GetReward() float32 {
	lockStat.Lock()
	defer lockStat.Unlock()
	res := float32(CommittedTxn)
	CommittedTxn = 0
	return res
}

func Send(level int, cid string, failure bool) {
	i := -1
	for j := 0; j < len(configs.OuAddress); j++ {
		if configs.OuAddress[j] == cid {
			i = j
		}
	}
	if level == 3 {
		i += 3
	}
	if configs.DetectorInitWaitCnt > 0 {
		Fixed[i].Send(level, cid, failure)
	} else {
		QT[i].Send(level, cid, failure)
	}
}

func Action(level int, cid string) int {
	i := -1
	for j := 0; j < len(configs.OuAddress); j++ {
		if configs.OuAddress[j] == cid {
			i = j
		}
	}
	if level == 3 {
		i += 3
	}
	if configs.DetectorInitWaitCnt > 0 {
		return Fixed[i].Action(cid)
	} else {
		res := QT[i].Action(cid)
		if res >= 0 {
			return res
		} else {
			// reinforcement learning have ended, get the answer.
			res = -res - 3
			if res < 1 {
				res = 1
			}
			configs.SetDown(res)
		}
		return QT[i].Action(cid)
	}
}
