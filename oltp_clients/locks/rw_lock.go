package locks

import (
	"sync"
	"time"
)

const (
	MaxTimeOut     = 60 * 60 * 1000 * time.Millisecond
	WriteProtectNs = 5 * 1000
)

func getTimeOut(t time.Duration) time.Duration {
	if t >= 0 {
		return t
	} else {
		return MaxTimeOut
	}
}

type RWLock struct {
	read                int
	write               int
	writeProtectEndTime int64
	prevStack           []byte
	mu                  sync.Mutex
}

func (c *RWLock) upgradeLock() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.write == 1 || c.read > 1 {
		// avoid write lock starvation caused by multiple read lock requests.
		c.writeProtectEndTime = time.Now().UnixNano() + WriteProtectNs
		return false
	}
	c.write = 1
	c.read = 0
	c.writeProtectEndTime = time.Now().UnixNano()
	return true
}

func (c *RWLock) UpgradeLock() bool {
	return c.upgradeLock()
}

func (c *RWLock) lock() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.write == 1 || c.read > 0 {
		// avoid write lock starvation caused by multiple read lock requests.
		c.writeProtectEndTime = time.Now().UnixNano() + WriteProtectNs
		return false
	}
	c.write = 1
	c.writeProtectEndTime = time.Now().UnixNano()
	return true
}

func (c *RWLock) TryLock() bool {
	return c.lock()
}

func (c *RWLock) Lock() {
	for !c.TryLock() {
	}
}

func (c *RWLock) Unlock() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.write = 0
}

func (c *RWLock) rLock() bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.write == 1 || time.Now().UnixNano() < c.writeProtectEndTime {
		return false
	}
	c.read += 1

	return true
}

// ClearOnce remove the strictest lock.
func (c *RWLock) ClearOnce() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.write == 1 {
		c.write = 0
	} else if c.read == 0 {
		panic("invalid clean for rw lock")
	} else {
		c.read--
	}
}

func (c *RWLock) TryRLock() bool {
	return c.rLock()
}

func (c *RWLock) RLock() {
	for !c.TryRLock() {
	}
}

func (c *RWLock) RUnlock() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.read > 0 {
		c.read--
	}
}

func NewLocker() *RWLock {
	return &RWLock{}
}
