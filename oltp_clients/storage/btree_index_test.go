package storage

import (
	"FC/configs"
	"fmt"
	"github.com/magiconair/properties/assert"
	"github.com/pingcap/go-ycsb/pkg/generator"
	"math/rand"
	"testing"
)

const testIndexSize = 1024 * 1024 * 1
const testInsertThreadNumber = 16
const testReadThreadNumber = 16

func TestBasicIndex(t *testing.T) {
	idx := NewBTree("test index")
	temp := &RowRecord{}
	err := idx.IndexInsert(1, temp)
	assert.Equal(t, nil, err)
	it, err := idx.IndexRead(1)
	assert.Equal(t, nil, err)
	assert.Equal(t, it, temp)
}

func indexInit(t *testing.T, idx *BTree, l int, r int) {
	keys := make([]Key, r-l+1)
	for i := l; i <= r; i++ {
		keys[i-l] = Key(i)
	}
	rand.Seed(233)
	rand.Shuffle(r-l+1, func(i, j int) {
		keys[i], keys[j] = keys[j], keys[i]
	})
	//fmt.Printf("%v\n", keys)
	//fmt.Printf("keys random, index insert start for %v-%v\n", l, r)
	retryStep := 0
	for i := l; i <= r; i++ {
		k := keys[i-l]
		//idx.PrintSubTree(idx.getRoot(), "")
		//fmt.Println("-----------------------------")
		//fmt.Println(keys[i-l])
		value := &RowRecord{PrimaryKey: k}
		err := idx.IndexInsert(k, value)
		for err == ErrAbort { // retry until succeed.
			err = idx.IndexInsert(k, value)
			retryStep++
		}
		assert.Equal(t, err, nil)
	}
	//fmt.Printf("keys random, index insert finished for %v-%v\n", l, r)
}

func indexInitParallel(t *testing.T, idx *BTree, size int, ch chan bool) {
	assert.Equal(t, 0, size%testInsertThreadNumber)
	for i := 0; i < testInsertThreadNumber; i++ {
		go func(i int, ch chan bool) {
			indexInit(t, idx, 1+size/testInsertThreadNumber*i, size/testInsertThreadNumber*(i+1))
			ch <- true
		}(i, ch)
	}
}

func indexAccessRoutine(t *testing.T, idx *BTree, size int, readCnt int, mustRead bool, finish chan bool, seed int64) {
	r := rand.New(rand.NewSource(seed))
	zip := generator.NewZipfianWithRange(1, int64(size), configs.YCSBDataSkewness)
	for i := 0; i < readCnt; i++ {
		key := Key(zip.Next(r))
		it, err := idx.IndexRead(key)
		for err == ErrAbort { // retry until succeed.
			it, err = idx.IndexRead(key)
		}
		if err == nil {
			assert.Equal(t, key, it.PrimaryKey)
		} else if mustRead {
			idx.PrintSubTree(idx.getRoot(), "")
			fmt.Println("KEY = ", key)
			assert.Equal(t, nil, err)
		} else if err != ErrKeyNotFound {
			assert.Equal(t, nil, err)
		}
	}
	finish <- true
}

func TestIndexInsertAndQuery(t *testing.T) {
	ch := make(chan bool)
	idx := NewBTree("test index")
	indexInit(t, idx, 1, testIndexSize)
	//idx.PrintSubTree(idx.getRoot(), "")
	go indexAccessRoutine(t, idx, testIndexSize, 5, true, ch, 123)
	<-ch
}

func TestConcurrentReadIndex(t *testing.T) {
	ch := make(chan bool)
	idx := NewBTree("test index")
	indexInit(t, idx, 1, testIndexSize)
	for i := 0; i < testReadThreadNumber; i++ {
		go indexAccessRoutine(t, idx, testIndexSize, 100000, true, ch, int64(i)*11+13)
	}
	for i := 0; i < testReadThreadNumber; i++ {
		<-ch
	}
}

func TestConcurrentInsertIndex(t *testing.T) {
	idx := NewBTree("test index")
	ch := make(chan bool)
	go indexInitParallel(t, idx, testIndexSize, ch)
	for i := 0; i < testInsertThreadNumber; i++ {
		<-ch
	}
}

func TestConcurrentInsertAndReadIndex(t *testing.T) {
	idx := NewBTree("test index")
	ch := make(chan bool)
	go indexInitParallel(t, idx, testIndexSize, ch)
	for i := 0; i < testReadThreadNumber; i++ {
		go indexAccessRoutine(t, idx, testIndexSize, 10000, false, ch, int64(i)*11+13)
	}
	for i := 0; i < testReadThreadNumber+testInsertThreadNumber; i++ {
		<-ch
	}
}
