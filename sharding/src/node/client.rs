extern crate users;
use crate::node::messages::message;

use super::super::utils::node_config::*;
use super::node::*;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::Arc;

/// This struct represents the Client node in the distributed system.
/// It finds the router and connects to it to send queries.
#[repr(C)]
pub struct Client {
    router_stream: TcpStream,
}

impl Client {
    /// Creates a new Client node with the given port
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        let config = get_router_config(config_path);
        let mut candidate_ip = String::new();
        let mut candidate_port = String::new();

        for node in config.nodes {
            let node_ip = node.ip.clone();
            let node_port = node.port.clone();

            // This shouldn't happen, but just in case
            if (&node_ip == ip) && (&node_port == port) {
                continue;
            }

            candidate_ip = node_ip.clone();
            candidate_port = node_port.clone();

            let mut candidate_stream =
                match TcpStream::connect(format!("{}:{}", candidate_ip, candidate_port)) {
                    Ok(stream) => stream,
                    Err(e) => {
                        eprintln!("Failed to connect to the router: {:?}", e);
                        panic!("Failed to connect to the router");
                    }
                };

            let message = message::Message::new(message::MessageType::GetRouter, None, None);
            candidate_stream
                .write(message.to_string().as_bytes())
                .unwrap();

            let response: &mut [u8] = &mut [0; 1024];
            candidate_stream
                .set_read_timeout(Some(std::time::Duration::from_secs(10)))
                .unwrap();

            match candidate_stream.read(response) {
                Ok(_) => {
                    let response_str = String::from_utf8_lossy(response);
                    let response_message = message::Message::from_string(&response_str).unwrap();
                    if response_message.message_type == message::MessageType::RouterId {
                        let node_info = response_message.node_info.unwrap();
                        let router_ip = node_info.ip.clone();
                        let router_port = node_info.port.clone();
                        let router_stream =
                            match TcpStream::connect(format!("{}:{}", router_ip, router_port)) {
                                Ok(stream) => stream,
                                Err(e) => {
                                    eprintln!("Failed to connect to the router: {:?}", e);
                                    panic!("Failed to connect to the router");
                                }
                            };
                        return Client { router_stream };
                    }
                }
                Err(_e) => {
                    continue;
                }
            }
        }

        panic!("No valid router found in the config");
    }
}

impl NodeRole for Client {
    fn send_query(&mut self, query: &str) -> bool {
        // TODO: Implement this
        false
    }
}
