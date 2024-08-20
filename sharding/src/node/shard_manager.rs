use std::{cmp::Ordering, collections::BinaryHeap, sync::{Arc, Mutex}};
use inline_colorization::*;

#[derive(Debug, Clone)]
pub(crate) struct ShardManager {
    shards: Arc<Mutex<BinaryHeap<ShardManagerObject>>>,
}

impl ShardManager {
    pub fn new() -> Self {
        ShardManager {
            shards: Arc::new(Mutex::new(BinaryHeap::new())),
        }
    }

    pub fn add_shard(&mut self, value: f64, shard_id: String) {
        let object = ShardManagerObject {
            key: value,
            value: shard_id,
        };
        println!("Adding shard: {:?}", object);
        let mut shards = self.shards.lock().unwrap();
        shards.push(object);
    }

    pub fn peek(&self) -> Option<String> {
        println!("Peeking shards: {:?}", self.shards);
        match self.shards.lock().unwrap().peek() {
            Some(object) => Some(object.value.clone()),
            None => None,
        }
    }

    pub fn update_shard_memory(&mut self, memory: f64, shard_id: String) {
        println!("{color_bright_green}Updating shard memory: {} to {}{style_reset}", shard_id, memory);
        self.pop();
        self.add_shard(memory, shard_id);

        println!("{color_bright_green}Shard memory updated: {:?}{style_reset}", self.shards);
    }

    fn pop(&mut self) -> Option<String> {
        match self.shards.lock().unwrap().pop() {
            Some(object) => Some(object.value),
            None => None,
        }
    }
}

#[derive(Debug)]
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
        self.key.partial_cmp(&other.key)
    }
}

impl PartialEq for ShardManagerObject {
    fn eq(&self, other: &Self) -> bool {
        self.key == other.key
    }
}

impl Eq for ShardManagerObject {}

#[cfg(test)]

mod tests {

    use super::*;

    #[test]
    fn test_shard_manager_init_empty() {
        let shard_manager = ShardManager::new();
        assert_eq!(shard_manager.peek(), None);
    }

    #[test]
    fn test_shard_manager_add_shard() {
        let mut shard_manager = ShardManager::new();
        shard_manager.add_shard(1.0, "shard1".to_string());
        assert_eq!(shard_manager.peek(), Some("shard1".to_string()));
    }

    #[test]
    fn test_shard_manager_add_multiple_shards_returns_max_shard() {
        let mut shard_manager = ShardManager::new();
        shard_manager.add_shard(1.0, "shard1".to_string());
        shard_manager.add_shard(2.0, "shard2".to_string());
        shard_manager.add_shard(3.0, "shard3".to_string());
        shard_manager.add_shard(4.0, "shard4".to_string());
        shard_manager.add_shard(5.0, "shard5".to_string());

        assert_eq!(shard_manager.peek(), Some("shard5".to_string()));
    }

    #[test]
    fn test_shard_manager_update_shard_memory_new_top() {
        let mut shard_manager = ShardManager::new();
        shard_manager.add_shard(1.0, "shard1".to_string());
        shard_manager.add_shard(2.0, "shard2".to_string());
        shard_manager.add_shard(3.0, "shard3".to_string());
        shard_manager.add_shard(4.0, "shard4".to_string());
        shard_manager.add_shard(5.0, "shard5".to_string());

        shard_manager.update_shard_memory(10.0, "shard3".to_string());

        assert_eq!(shard_manager.peek(), Some("shard3".to_string()));
    }

    #[test]
    fn test_shard_manager_update_shard_memory_from_top() {
        let mut shard_manager = ShardManager::new();
        shard_manager.add_shard(1.0, "shard1".to_string());
        shard_manager.add_shard(2.0, "shard2".to_string());
        shard_manager.add_shard(3.0, "shard3".to_string());
        shard_manager.add_shard(4.0, "shard4".to_string());
        shard_manager.add_shard(5.0, "shard5".to_string());

        shard_manager.update_shard_memory(0.0, "shard5".to_string());

        assert_eq!(shard_manager.peek(), Some("shard4".to_string()));
    }

    #[test]
    fn test_shard_manager_update_shard_memory_from_top_and_pop() {
        let mut shard_manager = ShardManager::new();
        shard_manager.add_shard(1.0, "shard1".to_string());
        shard_manager.add_shard(2.0, "shard2".to_string());
        shard_manager.add_shard(3.0, "shard3".to_string());
        shard_manager.add_shard(4.0, "shard4".to_string());
        shard_manager.add_shard(5.0, "shard5".to_string());

        shard_manager.update_shard_memory(0.0, "shard5".to_string());
        shard_manager.pop();

        assert_eq!(shard_manager.peek(), Some("shard3".to_string()));
    }

    #[test]
    fn test_shard_manager_pop() {
        let mut shard_manager = ShardManager::new();
        shard_manager.add_shard(1.0, "shard1".to_string());
        shard_manager.add_shard(2.0, "shard2".to_string());
        shard_manager.add_shard(3.0, "shard3".to_string());
        shard_manager.add_shard(4.0, "shard4".to_string());
        shard_manager.add_shard(5.0, "shard5".to_string());

        shard_manager.pop();
        assert_eq!(shard_manager.peek(), Some("shard4".to_string()));
    }
}