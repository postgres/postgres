package detector

import (
	"FC/configs"
	"time"
)

const AccessInterval time.Duration = 10 * time.Millisecond

func (c *LevelStateMachine) downTrans(initLevel Level) {
	// detector to the first Level.
	if c.Level <= initLevel {
		// conflict solve, can see paper.
		configs.LPrintf("downnnnnnnn!!!!!" + string(rune(c.Id)))
		c.Level = NoCFNoNF
		TimeStamp4NFRec++
	}
}

func (c *LevelStateMachine) getAction(curLevel Level, cid string, failure bool) int {
	// When Level changed, it would get reseted.
	Send(int(curLevel), cid, failure)
	return Action(int(curLevel), cid)
}

func (c *LevelStateMachine) Trans(curLevel Level, failure bool, cid string) {
	c.H = c.getAction(curLevel, cid, failure)
	if c.H == 0 {
		c.downTrans(curLevel)
	}
}
