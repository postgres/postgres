package locks

import (
	"fmt"
	"sync"
	"testing"
)

const concurrentThreadNumber = 8

func TestLatchExclusive(t *testing.T) {
	lock := NewLocker()
	x := 1
	wait := sync.WaitGroup{}
	for i := 0; i < concurrentThreadNumber; i++ {
		go func(i int, x *int, lock *RWLock) {
			for t := 0; t < 10; t++ {
				lock.Lock()
				*x = i
				lock.Unlock()
			}
			wait.Done()
		}(i, &x, lock)
		wait.Add(1)
	}
	wait.Wait()
}

func TestLatchShare(t *testing.T) {
	lock := NewLocker()
	x := 1
	wait := sync.WaitGroup{}
	for i := 0; i < concurrentThreadNumber; i++ {
		go func(i int, x *int, lock *RWLock) {
			for t := 0; t < 10; t++ {
				lock.RLock()
				_ = fmt.Sprint(*x)
				lock.RUnlock()
			}
			wait.Done()
		}(i, &x, lock)
		wait.Add(1)
	}
	wait.Wait()
}

func TestLatchMixed(t *testing.T) {
	lock := NewLocker()
	x := 1
	wait := sync.WaitGroup{}
	for i := 0; i < concurrentThreadNumber; i++ {
		go func(i int, x *int, lock *RWLock) {
			for t := 0; t < 100; t++ {
				lock.RLock()
				_ = fmt.Sprint(*x)
				lock.RUnlock()
			}
			wait.Done()
		}(i, &x, lock)
		wait.Add(1)
		go func(i int, x *int, lock *RWLock) {
			for t := 0; t < 100; t++ {
				for !lock.TryLock() {
				}
				*x = i
				lock.Unlock()
			}
			wait.Done()
		}(i, &x, lock)
		wait.Add(1)
	}
	wait.Wait()
}
