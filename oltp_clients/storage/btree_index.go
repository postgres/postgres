package storage

import (
	"FC/configs"
	"FC/locks"
	"errors"
	"fmt"
)

const (
	IndexNone   = 0
	IndexUpdate = 1
	IndexRead   = 2
	//IndexScan   = 3
)

var (
	ErrPointerHashReachedLimit = errors.New("the pointer has reached the limit, no more item")
	ErrShouldBeLeaf            = errors.New("current node should be the leaf node")
	ErrKeyNotFound             = errors.New("the key is not found in B-tree")
	ErrAbort                   = errors.New("transaction abort")
	ErrNodeIsNotFound          = errors.New("a child node is not found")
)

type Node struct {
	lock   *locks.RWLock
	size   uint32
	isLeaf bool
	data   []*RowRecord
	keys   []Key
	maxi   Key
	next   *Node
	parent *Node

	pointers []*Node

	from *BTree
}

func (c *Node) Iterator(key Key) (*Iterator, error) {
	it := &Iterator{key: key, offset: 0, node: c}
	if !c.isLeaf {
		return it, nil
	}
	for i := uint32(0); i < c.size; i++ {
		if c.keys[i] == key {
			it.offset = i
			return it, nil
		}
	}
	return it, ErrKeyNotFound
}

type BTree struct {
	order     uint32
	root      *Node
	indexName string
	rootLatch *locks.RWLock
}

func NewBTree(indexName string) *BTree {
	tree := &BTree{}
	tree.order = configs.BTreeOrder
	tree.root = tree.NewNode(true)
	tree.rootLatch = locks.NewLocker()
	tree.indexName = indexName
	return tree
}

func (t *BTree) setRoot(node *Node) {
	t.rootLatch.Lock()
	defer t.rootLatch.Unlock()
	t.root = node
}

func (t *BTree) getRoot() *Node {
	t.rootLatch.RLock()
	defer t.rootLatch.RUnlock()
	return t.root
}

func (t *BTree) NewNode(asLeaf bool) *Node {
	res := &Node{from: t, isLeaf: asLeaf, size: 0, lock: locks.NewLocker(), next: nil, parent: nil}
	res.keys = make([]Key, t.order, t.order)
	if asLeaf {
		res.data = make([]*RowRecord, t.order, t.order)
		res.pointers = nil
	} else {
		res.pointers = make([]*Node, t.order, t.order) // one more for >
		res.data = nil
	}
	return res
}

func (t *BTree) clearLock4Path(from *Node, to *Node) { // clear the locks on tree path from.parent -> to.
	if to == nil {
		return
	} else {
		for from != to {
			from = from.parent
			if from == nil {
				break
			}
			from.lock.ClearOnce()
		}
	}
}

func (t *BTree) findLeaf(key Key, accessType Access) (*Node, *Iterator, error) {
	c := t.getRoot()
	var splitNode *Node = nil
	splitNode = nil
	var i uint32
	if accessType == IndexNone { // lock-free access to the index.
		for !c.isLeaf {
			for i = 0; i < c.size && c.keys[i] < key; i++ {
			}
			c = c.pointers[i]
		}
		it, err := c.Iterator(key)
		if err != nil {
			return nil, nil, err
		}
		return splitNode, it, nil
	}
	if !c.lock.TryRLock() {
		return splitNode, nil, ErrAbort
	}
	for !c.isLeaf {
		for i = 0; i < c.size && c.keys[i] < key; i++ {
		}
		child := c.pointers[i]
		configs.Assert(child.parent == c, "parent pointer is not updated on time")
		if child == nil {
			return splitNode, nil, ErrNodeIsNotFound
		}
		if !child.lock.TryRLock() {
			t.clearLock4Path(c, splitNode)
			splitNode = nil
			c.lock.RUnlock()
			return splitNode, nil, ErrAbort
		}
		if accessType == IndexRead { // release lock directly for read accesses.
			c.lock.RUnlock()
		} else { // for insert operation, the split of leaf node will trigger its parent to split only when its child number order-1.
			// The nodes need to split forms a path from leaf to splitNode. Thus, we only keep locks from the leaf to the split node.
			if child.size == t.order-1 {
				if !c.lock.UpgradeLock() {
					t.clearLock4Path(c, splitNode)
					c.lock.RUnlock()
					child.lock.RUnlock()
					splitNode = nil
					return splitNode, nil, ErrAbort
				}
				if splitNode == nil {
					splitNode = c
				}
			} else {
				t.clearLock4Path(c, splitNode)
				c.lock.RUnlock()
				splitNode = nil
			}
		}
		c = child
	}

	if !c.isLeaf {
		panic(ErrShouldBeLeaf)
	}
	if accessType == IndexUpdate && !c.lock.UpgradeLock() {
		// the leaf node should always get latched for newly inserted Value.
		t.clearLock4Path(c, splitNode)
		c.lock.RUnlock()
		return splitNode, nil, ErrAbort
	}
	it, err := c.Iterator(key)
	if err != nil && !(err == ErrKeyNotFound && accessType == IndexUpdate) {
		t.clearLock4Path(c, splitNode)
		c.lock.RUnlock()
		return nil, nil, err
	}
	return splitNode, it, nil
}

func (t *BTree) IndexRead(key Key) (*RowRecord, error) {
	_, itr, err := t.findLeaf(key, IndexRead)
	if err != nil {
		return nil, err
	} else {
		res := itr.Value()
		itr.Free()
		return res, nil
	}
}

//func (t *BTree) IndexScan(fromKey Key, toKey Key) ([]*RowRecord, error) {
//	// TODO: implement index scan
//	return nil, nil
//}

func (t *BTree) IndexInsert(key Key, value *RowRecord) error {
	splitNode, it, err := t.findLeaf(key, IndexUpdate)
	if err != nil {
		return err
	}
	return t.insertIntoLeaf(it, key, value, splitNode)
}

func (t *BTree) createNewRoot(left *Node, right *Node) error {
	// If current node has been the leaf. create a new root (a new level) since current tree cannot hold all data.
	newRoot := t.NewNode(false)
	newRoot.keys[0] = left.maxi
	newRoot.pointers[0] = left
	newRoot.pointers[1] = right
	newRoot.maxi = right.maxi
	newRoot.size++
	configs.Assert(newRoot.size < t.order, "too many nodes in new root")
	left.next = right
	left.parent = newRoot
	right.parent = newRoot
	t.setRoot(newRoot)
	return nil
}

func (c *Node) min() Key {
	return c.keys[0]
}

func (c *Node) cutRightFrom(fullNode *Node) {
	configs.Assert(fullNode.size == c.from.order-1, "trying to split a not full leaf")
	fullNode.size = c.from.order / 2 // ceiling to the old node
	if !c.isLeaf {
		copy(c.keys, fullNode.keys[fullNode.size+1:])
		copy(c.pointers, fullNode.pointers[fullNode.size+1:])
		c.size = c.from.order - fullNode.size - 2
		// cannot update using the maxi of children, can cause concurrent bug
		c.maxi = fullNode.maxi
		fullNode.maxi = fullNode.pointers[fullNode.size].maxi
		for i := uint32(0); i <= c.size; i++ {
			// update parent pointers, new node and thus no concurrent access.
			c.pointers[i].parent = c
		}
	} else {
		copy(c.keys, fullNode.keys[fullNode.size:])
		copy(c.data, fullNode.data[fullNode.size:])
		c.size = c.from.order - fullNode.size - 1
		c.maxi = fullNode.maxi
		fullNode.maxi = fullNode.keys[fullNode.size-1]
	}
}

func (c *Node) merge(insertPoint uint32, key Key, value interface{}) {
	if !c.isLeaf {
		cur := value.(*Node)
		cur.parent = c
		for i := c.size; i > insertPoint; i-- {
			c.keys[i] = c.keys[i-1]
			c.pointers[i+1] = c.pointers[i]
		}
		c.keys[insertPoint] = key
		c.pointers[insertPoint+1] = cur
		c.pointers[insertPoint+1].parent = c
		if insertPoint == c.size {
			// only update when insert the max point, otherwise would cause concurrency problem.
			c.maxi = cur.maxi
		}
		c.size++
	} else {
		for i := c.size; i > insertPoint; i-- {
			c.keys[i] = c.keys[i-1]
			c.data[i] = c.data[i-1]
		}
		c.keys[insertPoint] = key
		c.data[insertPoint] = value.(*RowRecord)
		if insertPoint == c.size {
			// only update when insert the max point, otherwise would result in concurrency problem.
			c.maxi = key
		}
		c.size++
	}
}

func (t *BTree) insertChild(cur *Node, child *Node, key Key, split **Node) error {
	insertPoint := uint32(0)
	for ; insertPoint < cur.size && cur.keys[insertPoint] < key; insertPoint++ {
	}
	child.parent = cur
	if cur.size < t.order-1 { // no need to change the structure anymore.
		cur.merge(insertPoint, key, child)
		if cur.size >= t.order {
			panic("too many keys in leaf")
		}
		if cur != *split {
			panic("the last node is not split node")
		}
		*split = nil
		//flags := cur.lock.Flags()
		//configs.Assert(flags == utils.WriteProtectFlag, fmt.Sprintf("invalid lock: %b", flags))
		configs.Assert(cur.isLeaf == false, "invalid: inserted into leaf node")
		cur.lock.Unlock()
		return nil
	}
	tempNode := t.NewNode(false)
	tempNode.cutRightFrom(cur)
	tempNode.lock.Lock()
	if insertPoint <= cur.size {
		cur.merge(insertPoint, key, child)
	} else {
		insertPoint -= cur.size + 1
		tempNode.merge(insertPoint, key, child)
	}

	var err error

	if cur.parent != nil {
		err = t.insertChild(cur.parent, tempNode, cur.maxi, split)
	} else {
		err = t.createNewRoot(cur, tempNode)
	}
	tempNode.lock.Unlock()
	cur.lock.Unlock()
	return err
}

func (t *BTree) insertIntoLeaf(it *Iterator, key Key, value *RowRecord, split *Node) error {
	if it.exist(key) {
		panic("we currently do not support multiple values for the same key")
	}
	leaf := it.node
	insertPoint := uint32(0)
	for ; insertPoint < leaf.size && leaf.keys[insertPoint] < key; insertPoint++ {
	}
	if leaf.size < t.order-1 {
		// the Value can be inserted into the node without causing node split.
		leaf.merge(insertPoint, key, value)
		if leaf.size >= t.order {
			panic("too many keys in leaf")
		}
		//flags := leaf.lock.Flags()
		//configs.Assert(flags == lock.WriteProtectFlag, fmt.Sprintf("invalid lock: %b", flags))
		//configs.Assert(leaf.isLeaf == true, "invalid: inserted into leaf node")
		leaf.lock.Unlock()
	} else {
		newLeaf := t.NewNode(true)
		newLeaf.lock.Lock()
		configs.Assert(leaf.size == t.order-1, "trying to split a not full leaf")
		newLeaf.cutRightFrom(leaf)
		if insertPoint <= leaf.size {
			leaf.merge(insertPoint, key, value)
		} else {
			insertPoint -= leaf.size
			newLeaf.merge(insertPoint, key, value)
		}
		leaf.next = newLeaf
		var err error
		if leaf.parent == nil {
			err = t.createNewRoot(leaf, newLeaf)
		} else {
			err = t.insertChild(leaf.parent, newLeaf, leaf.maxi, &split)
		}
		leaf.lock.Unlock()
		newLeaf.lock.Unlock()
		if err != nil {
			return err
		}
	}
	return nil
}

type Iterator struct {
	node   *Node
	offset uint32
	key    Key
}

// Next does not ensure cross node consistency.
func (it *Iterator) Next() error {
	if it.offset+1 >= it.node.size {
		it.node.lock.RUnlock()
		it.node.lock.RLock()
		it.node = it.node.next
		it.offset = 0
	}
	if it.node == nil {
		return ErrPointerHashReachedLimit
	}
	if !it.node.isLeaf {
		return ErrShouldBeLeaf
	}
	return nil
}

func (it *Iterator) Value() *RowRecord {
	return it.node.data[it.offset]
}

func (it *Iterator) Free() {
	it.node.lock.RUnlock()
}

func (it *Iterator) exist(key Key) bool {
	for i := uint32(0); i < it.node.size; i++ {
		if it.node.keys[i] == key {
			return true
		}
	}
	return false
}

func (t *BTree) PrintSubTree(cur *Node, prev string) {
	if cur.isLeaf {
		fmt.Printf(prev+"[%v #%v]\n", cur.keys[:cur.size], cur.size)
		return
	}
	for i := uint32(0); i <= cur.size; i++ {
		t.PrintSubTree(cur.pointers[i], prev+"--")
		if i < cur.size {
			fmt.Printf(prev+"->"+"%v\n", cur.keys[i])
		}
	}
}
