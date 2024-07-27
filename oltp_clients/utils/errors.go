package utils

import "errors"

// These errors can occur for when using TryLock.
var (
	ErrLockTimeout = errors.New("get lock timeout")
	ErrTimeout     = errors.New("timeout")
)
