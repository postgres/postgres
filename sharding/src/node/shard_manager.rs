use std::{cmp::Ordering, collections::BinaryHeap};

pub(crate) struct ShardManager {
    shards: BinaryHeap<ShardManagerObject>,
}

impl ShardManager {
    pub fn new() -> Self {
        ShardManager {
            shards: BinaryHeap::new(),
        }
    }

    pub fn add_shard(&mut self, value: f64, shard_id: String) {
        let inverted = value.powf(-1.0);
        let object = ShardManagerObject {
            key: inverted,
            value: shard_id,
        };
        self.shards.push(object);
    }

    pub fn peek(&self) -> Option<&String> {
        match self.shards.peek() {
            Some(object) => Some(&object.value),
            None => None,
        }
    }

    pub fn pop(&mut self) -> Option<String> {
        match self.shards.pop() {
            Some(object) => Some(object.value),
            None => None,
        }
    }
}

struct ShardManagerObject {
    key: f64,
    value: String,
}

impl Ord for ShardManagerObject {
    fn cmp(&self, other: &Self) -> Ordering {
        self.partial_cmp(other).unwrap()
    }
}

impl PartialOrd for ShardManagerObject {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        other.key.partial_cmp(&self.key)
    }
}

impl PartialEq for ShardManagerObject {
    fn eq(&self, other: &Self) -> bool {
        self.key == other.key
    }
}

impl Eq for ShardManagerObject {}