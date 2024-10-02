use inline_colorization::*;
use postgres::Row;
extern crate users;
use std::{
    io::{Read, Write},
    net::TcpStream,
    sync::{Arc, Mutex},
};

use super::super::utils::node_config::*;
use super::node::*;
use crate::utils::common::Channel;
use crate::{
    node::messages::{message, node_info::NodeInfo},
    utils::queries::print_query_response,
};

/// This struct represents the Client node in the distributed system.
/// It finds the router and connects to it to send queries.
#[repr(C)]
#[derive(Clone)]
pub struct Client {
    router_postgres_client: Channel,
    client_info: NodeInfo,
}

impl Client {
    /// Creates a new Client node with the given port
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        let config = get_router_config(config_path);
        let mut candidate_ip;
        let mut candidate_port;

        for node in config.nodes {
            candidate_ip = node.ip.clone();
            candidate_port = node.port.clone().parse::<u64>().unwrap() + 1000;

            // This shouldn't happen, but just in case
            if (&candidate_ip == ip) && (&candidate_port.to_string() == port) {
                continue;
            }

            let mut candidate_stream =
                match TcpStream::connect(format!("{}:{}", candidate_ip, candidate_port)) {
                    Ok(stream) => {
                        println!(
                    "{color_bright_green}Health connection established with {}:{}{style_reset}",
                    candidate_ip, candidate_port
                );
                        stream
                    }
                    Err(e) => {
                        eprintln!("Failed to connect to the router: {:?}", e);
                        continue;
                    }
                };

            let message = message::Message::new_get_router();
            candidate_stream
                .write_all(message.to_string().as_bytes())
                .unwrap();

            let response: &mut [u8] = &mut [0; 1024];
            candidate_stream
                .set_read_timeout(Some(std::time::Duration::from_secs(10)))
                .unwrap();

            match candidate_stream.read(response) {
                Ok(_) => {
                    let response_str = String::from_utf8_lossy(response);
                    let response_message = message::Message::from_string(&response_str).unwrap();

                    if response_message.get_message_type() == message::MessageType::RouterId {
                        let node_info: NodeInfo = response_message.get_data().node_info.unwrap();
                        let node_ip = node_info.ip.clone();
                        let node_port = node_info.port.clone();
                        let connections_port = node_port.parse::<u64>().unwrap() + 1000;
                        let router_stream =
                            match TcpStream::connect(format!("{}:{}", node_ip, connections_port)) {
                                Ok(stream) => {
                                    println!(
                                        "{color_bright_green}Router stream {}:{}{style_reset}",
                                        node_ip,
                                        connections_port.to_string()
                                    );
                                    stream
                                }
                                Err(e) => {
                                    eprintln!("Failed to connect to the router: {:?}", e);
                                    panic!("Failed to connect to the router");
                                }
                            };

                        return Client {
                            router_postgres_client: Channel {
                                stream: Arc::new(Mutex::new(router_stream)),
                            },
                            client_info: NodeInfo {
                                ip: ip.to_string(),
                                port: port.to_string(),
                            },
                        };
                    }
                }
                Err(_e) => {
                    continue;
                }
            }
        }

        panic!("No valid router found in the config");
    }

    fn handle_received_message(buffer: &mut [u8]) {
        let message_string = String::from_utf8_lossy(&buffer);
        let response_message = message::Message::from_string(&message_string).unwrap();

        if response_message.get_message_type() == message::MessageType::QueryResponse {
            let rows = response_message.get_data().query.unwrap();
            print_query_response(rows);
        }
    }
}

impl NodeRole for Client {
    fn send_query(&mut self, query: &str) -> Option<String> {
        let message =
            message::Message::new_query(Some(self.client_info.clone()), query.to_string());
        let mut stream = self.router_postgres_client.stream.lock().unwrap();
        stream.write_all(message.to_string().as_bytes()).unwrap();

        let mut buffer: [u8; 1024] = [0; 1024];

        match stream.read(&mut buffer) {
            Ok(chars) => {
                if chars == 0 {
                    return None;
                }

                Client::handle_received_message(&mut buffer);
                Some(String::new())
            }
            Err(_e) => None,
        }
    }
}
