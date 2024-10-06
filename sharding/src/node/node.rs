use crate::node::client::Client;

use super::router::Router;
use super::shard::Shard;
use std::ffi::CStr;
use std::sync::{Arc, Mutex};
use std::thread;

/// The role of a node in the sharding system
pub trait NodeRole {
    /// Sends a query to the shard group
    fn send_query(&mut self, query: &str) -> Option<String>;
}

#[repr(C)]
#[derive(Debug)]
pub enum NodeType {
    Client,
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
pub fn get_node_instance() -> &'static mut dyn NodeRole {
    unsafe {
        NODE_INSTANCE
            .as_mut()
            .unwrap()
            .instance
            .as_mut()
            .unwrap()
            .as_mut()
    }
}

#[no_mangle]
pub extern "C" fn init_node_instance(
    node_type: NodeType,
    port: *const i8,
    config_file_path: *const i8,
) {
    unsafe {
        if port.is_null() {
            panic!("Received a null pointer for port");
        }

        let port_str = CStr::from_ptr(port);
        let node_port = match port_str.to_str() {
            Ok(str) => str,
            Err(_) => {
                panic!("Received an invalid UTF-8 string");
            }
        };

        let config_path = parse_config_file(config_file_path);

        let ip = "127.0.0.1";

        match node_type {
            NodeType::Router => {
                let router = Router::new(ip, node_port, config_path);
                NODE_INSTANCE = Some(NodeInstance::new(Box::new(router.clone())));

                let shared_router: Arc<Mutex<Router>> = Arc::new(Mutex::new(router));

                let _handle = thread::spawn(move || {
                    Router::wait_for_client(shared_router, ip, node_port);
                });

                println!("Router node initializes");
            }
            NodeType::Shard => {
                println!("Sharding node initializing");
                let shard = Shard::new(ip, node_port);
                NODE_INSTANCE = Some(NodeInstance::new(Box::new(shard.clone())));

                let shared_shard = Arc::new(Mutex::new(shard));

                let _handle = thread::spawn(move || {
                    Shard::accept_connections(shared_shard, ip, node_port);
                });

                println!("Sharding node initializes");
            }
            NodeType::Client => {
                println!("Client node initializing");
                NODE_INSTANCE = Some(NodeInstance::new(Box::new(Client::new(
                    ip,
                    node_port,
                    config_path,
                ))));
                println!("Client node initializes");
            }
        }
    }
}

fn parse_config_file(config_file_path: *const i8) -> Option<&'static str> {
    match config_file_path.is_null() {
        true => None,
        false => unsafe {
            let config_path_str = CStr::from_ptr(config_file_path);
            let config_path = match config_path_str.to_str() {
                Ok(str) => str,
                Err(_) => {
                    panic!("Received an invalid UTF-8 string for config path");
                }
            };
            Some(config_path)
        },
    }
}
