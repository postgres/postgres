use inline_colorization::*;
use postgres::{Client, NoTls};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, RwLock};
use std::{io, thread};

extern crate users;
use super::memory_manager::MemoryManager;
use super::node::*;
use crate::utils::node_config::get_shard_config;
use rust_decimal::Decimal;
use users::get_current_username;

/// This struct represents the Shard node in the distributed system. It will communicate with the router
#[repr(C)]
pub struct Shard<'a> {
    // router: Client,
    backend: Client,
    ip: &'a str,
    port: &'a str,
    listener: Arc<RwLock<TcpListener>>,
    router_stream: Arc<RwLock<Option<TcpStream>>>,
    memory_manager: MemoryManager,
}

impl<'a> Shard<'a> {
    /// Creates a new Shard node with the given port
    pub fn new(ip: &'a str, port: &'a str) -> Self {
        println!("Creating a new Shard node with port: {}", port);

        // get username dynamically
        let username = match get_current_username() {
            Some(username) => username.to_string_lossy().to_string(),
            None => panic!("Failed to get current username"),
        };
        println!("Username found: {:?}", username);
        println!("Connecting to the database with port: {}", port);

        let mut backend: Client = match Client::connect(
            format!(
                "host=127.0.0.1 port={} user={} dbname=template1",
                port, username
            )
            .as_str(),
            NoTls,
        ) {
            Ok(backend) => backend,
            Err(e) => {
                eprintln!("Failed to connect to the database: {:?}", e);
                panic!("Failed to connect to the database");
            }
        };
        let mut listener = Arc::new(RwLock::new(
            TcpListener::bind(format!("{}:{}", ip, port.parse::<u64>().unwrap() + 1000)).unwrap(),
        ));
        let mut stream = Arc::new(RwLock::new(None));
        let mut listener_clone = listener.clone();
        let mut stream_clone = stream.clone();

        let handle = thread::spawn(move || {
            Self::accept_connections(listener_clone, stream_clone);
        });

        // Initialize memory manager
        let config = get_shard_config();
        let memory_threshold = config.memory_threshold;
        let memory_manager = MemoryManager::new(memory_threshold);

        println!(
            "{color_blue}[Shard] Available Memory: {:?} %{style_reset}",
            memory_manager.available_memory_perc
        );
        println!(
            "{color_blue}[Shard] Accepts Insertions: {:?}{style_reset}",
            memory_manager.accepts_insertions()
        );

        Shard {
            // router: clients,
            backend,
            ip,
            port,
            listener,
            router_stream: stream.clone(),
            memory_manager,
        }
    }

    fn accept_connections(
        listener: Arc<RwLock<TcpListener>>,
        rw_stream: Arc<RwLock<Option<TcpStream>>>,
    ) {
        let listener_guard = listener.read().unwrap();
        match listener_guard.accept() {
            Ok((stream, addr)) => {
                println!("New connection accepted from {}.", addr);
                let mut stream_guard = rw_stream.write().unwrap();
                *stream_guard = Some(stream);
            }
            Err(e) => {
                eprintln!("Failed to accept a connection: {}", e);
            }
        }
    }

    // Listen for incoming messages
    // TODO-SHARD: Call this function
    pub fn listen(&mut self) {
        let mut stream = self.router_stream.read().unwrap();
        loop {
            // TODO-SHARD fix this multiple match statements so they're not nested
            let message = match stream.as_ref() {
                Some(stream) => {
                    let mut buffer = [0; 1024];
                    match stream.read(&mut buffer) {
                        Ok(_) => {
                            // Check if it's a AskInsertion message. If it is, send Agreed if self.accepts_insertions() is true, else send Denied, or Denied if self.accepts_insertions() is false
                            match Message::from_string(&String::from_utf8_lossy(&buffer)) {
                                Ok(message) => {
                                    match message.message_type {
                                        MessageType::AskInsertion => {
                                            let response = self.get_insertion_response();
                                            stream.write_all(response.to_string().as_bytes()).unwrap();
                                        }
                                        _ => {
                                            eprintln!("Invalid message type");
                                        }
                                    }
                                }
                                Err(e) => {
                                    eprintln!("Failed to parse message: {}", e);
                                }
                            }

                        }
                        Err(e) => {
                            eprintln!("Failed to read from stream: {}", e);
                        }
                    }
                }
                None => {
                    eprintln!("Stream is not available");
                }
            }
        }
    }

    pub fn update(&mut self) -> Result<(), io::Error> {
        self.memory_manager.update()
    }

    fn accepts_insertions(&self) -> bool {
        self.memory_manager.accepts_insertions()
    }

    fn get_insertion_response(&self) -> MessageType {
        if self.accepts_insertions() {
            MessageType::Agreed
        } else {
            MessageType::Denied
        }
    }
}

impl<'a> NodeRole for Shard<'a> {
    fn send_query(&mut self, query: &str) -> bool {
        let rows = match self.backend.query(query, &[]) {
            Ok(rows) => {
                println!("Query executed successfully: {:?}", rows);
                rows
            }
            Err(e) => {
                eprintln!("Failed to execute query: {:?}", e);
                return false;
            }
        };

        // for row in rows {
        //     let id: i32 = row.get(0);
        //     let name: &str = row.get(1);
        //     let position: &str = row.get(2);
        //     let salary: Decimal = row.get(3);
        //     println!(
        //         "QUERY RESULT: id: {}, name: {}, position: {}, salary: {}",
        //         id, name, position, salary
        //     );
        // }

        true
    }
}
