use crate::node::client::Client;
use crate::utils::node_config::get_nodes_config_raft;

use super::router::Router;
use super::shard::Shard;
use std::ffi::CStr;
use std::sync::{Arc, Mutex};
use std::thread;
use futures::executor::block_on;
use raft::raft_module::RaftModule;
use tokio::runtime::Runtime;
use actix_rt::System;

use tokio::task;
use tokio::task::LocalSet;

pub trait NodeRole {
    /// Sends a query to the shard group
    fn send_query(&mut self, query: &str) -> Option<String>;
}

#[repr(C)]
#[derive(Debug, PartialEq)]
pub enum NodeType {
    Client,
    Router,
    Shard,
}

/* Node Singleton */

pub struct NodeInstance {
    pub instance: Option<Box<dyn NodeRole>>,
    pub ip: String,
    pub port: String,
}

impl NodeInstance {
    fn new(instance: Box<dyn NodeRole>, ip: String, port: String) -> Self {
        NodeInstance {
            instance: Some(instance),
            ip,
            port
        }
    }

    fn change_role(&mut self, new_role: NodeType) {
        let current_instance: &mut Box<dyn NodeRole> = self.instance.as_mut().unwrap();

        match new_role {
            NodeType::Router => {
                // TODO-A: Implement data migration to another shard
                let router = Router::new(&self.ip, &self.port, None);
                *current_instance = Box::new(router);
            }
            NodeType::Shard => {
                let shard = Shard::new(&self.ip, &self.port);
                *current_instance = Box::new(shard);
            }
            _ => {
                panic!("NodeRole can only be changed to Router or Shard");
            }
        }
    }
}

pub static mut NODE_INSTANCE: Option<NodeInstance> = None;

pub fn get_node_role() -> &'static mut dyn NodeRole {
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

// External use of Node Instance
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

        new_node_instance(node_type, ip, node_port, config_path);
    }
}




fn new_node_instance(node_type: NodeType, ip: &str, port: &str, config_file_path: Option<&str>) {
    // Initialize node based on node type
    match node_type {
        NodeType::Router => init_router(ip, port, config_file_path),
        NodeType::Shard => init_shard(ip, port),
        NodeType::Client => init_client(ip, port, config_file_path),
    }

    // Define node_id and create the Raft module instance
    let node_id = format!("{}:{}", ip, port);
    let mut raft_module = raft::raft_module::RaftModule::new(
        node_id.clone(),
        ip.to_string(),
        port.parse::<usize>().unwrap(),
    );
    let nodes = get_nodes_config_raft(config_file_path);

    // Run everything within Actix runtime
    System::new().block_on(async move {
        println!("Starting Raft module");

        // Spawn the Raft task
        actix_rt::spawn(async move {
            raft_module
                .start(nodes, Some(&format!("../../../sharding/init_history/init_{}", node_id)))
                .await;
        }).await.expect("Error in Raft");
    });
}




fn init_router(ip: &str, port: &str, config_file_path: Option<&str>) {
    let router = Router::new(ip, port, config_file_path);
    
    unsafe {
        NODE_INSTANCE = Some(NodeInstance::new(
            Box::new(router.clone()),
            ip.to_string(),
            port.to_string()
        ));
    }

    let shared_router: Arc<Mutex<Router>> = Arc::new(Mutex::new(router));
    let ip_clone = ip.to_string();
    let port_clone = port.to_string();
    let _handle = thread::spawn(move || {
        Router::wait_for_client(shared_router, ip_clone, port_clone);
    });

    println!("Router node initializes");
}

fn init_shard(ip: &str, port: &str) {
    println!("Sharding node initializing");
    let shard = Shard::new(ip, port);
    
    unsafe {
        NODE_INSTANCE = Some(NodeInstance::new(
            Box::new(shard.clone()),
            ip.to_string(),
            port.to_string()
        ));
    }

    let shared_shard = Arc::new(Mutex::new(shard));
    let ip_clone = ip.to_string();
    let port_clone = port.to_string();
    let _handle = thread::spawn(move || {
        Shard::accept_connections(shared_shard, ip_clone, port_clone);
    });

    println!("Sharding node initializes");
}

fn init_client(ip: &str, port: &str, config_file_path: Option<&str>) {
    println!("Client node initializing");
    unsafe {
        NODE_INSTANCE = Some(NodeInstance::new(
            Box::new(Client::new(ip, port, config_file_path)),
            ip.to_string(),
            port.to_string()
        ));
    }
    println!("Client node initializes");
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
