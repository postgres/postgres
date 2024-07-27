package learner

import "time"

func GetDelayVector(shards []string) map[string]time.Duration {
	res := make(map[string]time.Duration)
	for _, v := range shards {
		res[v] = getExpectedDelay(v)
	}
	return res
}

func GetParticipantDivide(shards []string) [][]string {
	return [][]string{shards[:1], shards[1:]}
	//return [][]string{shards}
}

func getExpectedDelay(participant string) time.Duration {
	return 0
}
