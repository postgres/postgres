use std::sync::{Arc, Mutex};
use std::cell::UnsafeCell;
use super::router::Router;

// TODO-SHARD this file should be a more organized configuration file
pub const FILE_PATH: &str = "ports.txt";

/// The role of a node in the sharding system
pub trait NodeRole {
    /// Sends a query to the shard group
    extern "C" fn send_query(&mut self, query: &str) -> bool;
}

#[repr(C)]
pub enum NodeType {
    Router,
    Shard,
}

/* Node Singleton */

pub struct NodeInstance {
    pub instance: Option<Box<dyn NodeRole>>,
}

impl NodeInstance {
    fn new(instance: Box<dyn NodeRole>) -> Self {
        NodeInstance {
            instance: Some(instance),
        }
    }
}

pub static mut NODE_INSTANCE: Option<NodeInstance> = None;

// This needs to return NodeRole
pub fn get_node_instance()  -> &'static mut dyn NodeRole {
    unsafe {
        NODE_INSTANCE.as_mut().unwrap().instance.as_mut().unwrap().as_mut()
    }
}

#[no_mangle]
pub extern "C" fn init_node_instance(node_type: NodeType, port: String) {
    unsafe {
        match node_type {
            NodeType::Router => {
                println!("Router node initializing");
                NODE_INSTANCE = Some(NodeInstance::new(Box::new(Router::new(port))));
                println!("Router node initializes");
            },
            NodeType::Shard => {
                // TODO-SHARD: implement Shard
            },
        }
    }
}