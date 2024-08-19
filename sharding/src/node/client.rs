extern crate users;
use super::super::utils::node_config::*;
use super::node::*;
use std::net::TcpStream;
use std::sync::Arc;
use std::sync::RwLock;

/// This struct represents the Client node in the distributed system.
/// It finds the router and connects to it to send queries.
#[repr(C)]
pub struct Client {
    router_stream: Arc<RwLock<Option<TcpStream>>>,
    ip: Arc<str>,
    port: Arc<str>,
}

impl Client {
    /// Creates a new Client node with the given port
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        let config = get_router_config(config_path);

        for node in config.nodes {
            let node_ip = node.ip;
            let node_port = node.port;

            // This shouldn't happen, but just in case
            if (node_ip == ip) && (node_port == port) {
                continue;
            }
        }
    }
}

impl NodeRole for Client {
    fn send_query(&mut self, query: &str) -> bool {
        // TODO: Implement this
        false
    }
    fn accepts_insertions(&self) -> bool {
        false
    }

    fn get_router_data(&self) -> (String, String) {
        (self.ip.to_string(), self.port.to_string())
    }
}
