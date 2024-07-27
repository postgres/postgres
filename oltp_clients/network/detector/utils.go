package detector

func MaxLevel(x Level, y Level) Level {
	if x > y {
		return x
	}
	return y
}

func Max(x int, y int) int {
	if x > y {
		return x
	}
	return y
}
