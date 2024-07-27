package detector

import (
	"FC/configs"
	"time"
)

type SimpleLearner struct {
	level int
	cnt   int
	react int
}

var Fixed []SimpleLearner

func (tes *SimpleLearner) Send(level int, cid string, failure bool) {
	if level == 1 {
		tes.react = 1
		return
	}
	if level != tes.level || failure {
		// reset
		tes.level = level
		tes.cnt = configs.DetectorInitWaitCnt
		tes.react = 1
	} else {
		// transition
		if tes.cnt == 0 {
			// back to initial Level
			tes.react = 0
			tes.level = 1
		} else {
			tes.react = 1
			tes.cnt--
		}
	}
}

func (tes *SimpleLearner) Action(cid string) int {
	for {
		rec := tes.react
		if rec == -1 {
			time.Sleep(5 * time.Millisecond)
			continue
		} else {
			tes.react = -1
			return int(rec)
		}
	}
}
