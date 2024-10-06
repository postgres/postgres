use indexmap::IndexMap;
use postgres::{Client as PostgresClient, Row};
extern crate users;
use super::node::*;

use super::shard_manager::ShardManager;
use super::tables_id_info::TablesIdInfo;
use crate::node::messages::message::{Message, MessageType};
use crate::node::messages::node_info::NodeInfo;
use crate::utils::common::ConvertToString;
use crate::utils::common::{connect_to_node, Channel};
use crate::utils::node_config::{get_router_config, Node};
use crate::utils::queries::{
    format_query_with_new_id, format_rows_with_offset, get_id_if_exists, get_table_name_from_query,
    print_query_response, query_affects_memory_state, query_is_insert, query_is_select,
};
use inline_colorization::*;
use std::io::{Read, Write};
use std::sync::{Arc, MutexGuard, RwLock};
use std::{io, net::TcpListener, net::TcpStream, sync::Mutex, thread};

/// This struct represents the Router node in the distributed system. It has the responsibility of routing the queries to the appropriate shard or shards.
#[repr(C)]
#[derive(Clone)]
pub struct Router {
    ///  IndexMap:
    ///     key: shardId
    ///     value: Shard's Client
    shards: Arc<Mutex<IndexMap<String, PostgresClient>>>,
    shard_manager: Arc<ShardManager>,
    ///  IndexMap:
    ///     key: Hash
    ///     value: shardId
    comm_channels: Arc<RwLock<IndexMap<String, Channel>>>,
    ip: Arc<str>,
    port: Arc<str>,
}

impl Router {
    /// Creates a new Router node with the given port and ip, connecting it to the shards specified in the configuration file.
    pub fn new(ip: &str, port: &str, config_path: Option<&str>) -> Self {
        Router::initialize_router_with_connections(ip, port, config_path)
    }

    pub fn wait_for_client(shared_router: Arc<Mutex<Router>>, ip: &str, port: &str) {
        let listener =
            TcpListener::bind(format!("{}:{}", ip, port.parse::<u64>().unwrap() + 1000)).unwrap();

        loop {
            match listener.accept() {
                Ok((stream, addr)) => {
                    println!(
                        "{color_bright_green}[ROUTER] New connection accepted from {}.{style_reset}",
                        addr
                    );

                    // Start listening for incoming messages in a thread
                    let router_clone = shared_router.clone();
                    let shareable_stream = Arc::new(Mutex::new(stream));
                    let stream_clone = Arc::clone(&shareable_stream);

                    let _handle = thread::spawn(move || {
                        Router::listen(router_clone, stream_clone);
                    });
                }
                Err(e) => {
                    eprintln!("Failed to accept a connection: {}", e);
                }
            }
        }
    }

    // Listen for incoming messages
    pub fn listen(shared_router: Arc<Mutex<Router>>, stream: Arc<Mutex<TcpStream>>) {
        loop {
            // sleep for 1 millisecond to allow the stream to be ready to read
            thread::sleep(std::time::Duration::from_millis(1));
            let mut router = shared_router.lock().unwrap();
            let mut buffer = [0; 1024];

            let mut stream = stream.lock().unwrap();

            match stream.set_read_timeout(Some(std::time::Duration::new(10, 0))) {
                Ok(_) => {}
                Err(_e) => {
                    continue;
                }
            }

            match stream.read(&mut buffer) {
                Ok(chars) => {
                    if chars == 0 {
                        continue;
                    }
                    let message_string = String::from_utf8_lossy(&buffer);
                    match router.get_response_message(&message_string) {
                        Some(response) => {
                            stream.write(response.as_bytes()).unwrap();
                        }
                        None => {
                            // do nothing
                        }
                    }
                }
                Err(_e) => {
                    // could not read from the stream, ignore
                }
            }
        }
    }

    fn get_response_message(&mut self, message: &str) -> Option<String> {
        if message.is_empty() {
            return None;
        }

        let message = match Message::from_string(&message) {
            Ok(message) => message,
            Err(e) => {
                eprintln!("Failed to parse message: {:?}. Message: [{:?}]", e, message);
                return None;
            }
        };

        match message.get_message_type() {
            MessageType::Query => {
                let query = message.get_data().query.unwrap();
                let response = match self.send_query(&query) {
                    Some(response) => response,
                    None => {
                        eprintln!("Failed to send query to shards");
                        return None;
                    }
                };
                let response_message = Message::new_query_response(response);
                Some(response_message.to_string())
            }
            _ => {
                eprintln!(
                    "Message type received: {:?}, not yet implemented",
                    message.get_message_type()
                );
                None
            }
        }
    }

    /// Initializes the Router node with connections to the shards specified in the configuration file.
    fn initialize_router_with_connections(
        ip: &str,
        port: &str,
        config_path: Option<&str>,
    ) -> Router {
        let config = get_router_config(config_path);
        let shards: IndexMap<String, PostgresClient> = IndexMap::new();
        let comm_channels: IndexMap<String, Channel> = IndexMap::new();
        let shard_manager = ShardManager::new();

        let mut router = Router {
            shards: Arc::new(Mutex::new(shards)),
            shard_manager: Arc::new(shard_manager),
            comm_channels: Arc::new(RwLock::new(comm_channels)),
            ip: Arc::from(ip),
            port: Arc::from(port),
        };

        for shard in config.nodes {
            if (shard.ip == router.ip.as_ref()) && (shard.port == router.port.as_ref()) {
                continue;
            }
            router.configure_shard_connection_to(shard)
        }
        router
    }

    /// Configures the connection to a shard with the given ip and port.
    fn configure_shard_connection_to(&mut self, node: Node) {
        let node_ip = node.ip;
        let node_port = node.port;

        let shard_client = match connect_to_node(&node_ip, &node_port) {
            Ok(shard_client) => shard_client,
            Err(_) => {
                println!("Failed to connect to port: {}", node_port);
                return;
            }
        };
        println!("Connected to ip {} and port: {}", node_ip, node_port);

        self.save_shard_client(node_port.to_string(), shard_client);
        self.set_health_connection(node_ip.as_str(), node_port.as_str());
    }

    /// Saves the shard client in the Router's shards IndexMap with its corresponding shard id as key.
    fn save_shard_client(&mut self, shard_id: String, shard_client: PostgresClient) {
        let mut shards = self.shards.lock().unwrap();
        shards.insert(shard_id, shard_client);
    }

    /// Sets the health_connection to the shard with the given ip and port, initializing the communication with a handshake between the router and the shard.
    fn set_health_connection(&mut self, node_ip: &str, node_port: &str) {
        let health_connection = match Router::get_shard_channel(&node_ip, &node_port) {
            Ok(health_connection) => health_connection,
            Err(_) => {
                println!("Failed to create health-connection to port: {}", node_port);
                return;
            }
        };
        if self.send_init_connection_message(health_connection.clone(), node_port) {
            self.save_comm_channel(node_port.to_string(), health_connection);
        }
    }

    /// Saves the communication channel to the shard with the given shard id as key.
    fn save_comm_channel(&mut self, shard_id: String, channel: Channel) {
        let mut comm_channels = self.comm_channels.write().unwrap();
        comm_channels.insert(shard_id, channel);
    }

    /// Sends the InitConnection message to the shard with the given shard id, initializing the communication with a handshake between the router and the shard. The shard will respond with a MemoryUpdate message, which will be handled by the router updating the shard's memory size in the ShardManager.
    fn send_init_connection_message(
        &mut self,
        health_connection: Channel,
        node_port: &str,
    ) -> bool {
        // Send InitConnection Message to Shard and save shard to ShardManager
        let mut stream = health_connection.stream.as_ref().lock().unwrap();

        let node_info = NodeInfo {
            ip: self.ip.as_ref().to_string(),
            port: self.port.as_ref().to_string(),
        };
        let update_message = Message::new_init_connection(node_info);
        println!("Sending message to shard: {:?}", update_message);

        let message_string = update_message.to_string();
        stream.write_all(message_string.as_bytes()).unwrap();

        println!("Waiting for response from shard");

        let response: &mut [u8] = &mut [0; 1024];

        // Wait for timeout and read response
        stream
            .set_read_timeout(Some(std::time::Duration::new(10, 0)))
            .unwrap();

        match stream.read(response) {
            Ok(_) => {
                let response_string = String::from_utf8_lossy(response);
                let response_message = Message::from_string(&response_string).unwrap();
                //println!("Response from shard: {:?}", response_message);
                self.handle_response(response_message, node_port)
            }
            Err(_) => {
                println!(
                    "{color_red}Shard {} did not respond{style_reset}",
                    node_port
                );
                false
            }
        }
    }

    /// Handles the responses from the shard from the health_connection channel.
    fn handle_response(&mut self, response_message: Message, node_port: &str) -> bool {
        match response_message.get_message_type() {
            MessageType::Agreed => {
                println!(
                    "{color_bright_green}Shard {} accepted the connection. Message: {:?}{style_reset}",
                    node_port, response_message.get_data()
                );
                let memory_size = response_message.get_data().payload.unwrap();
                let max_ids_info = response_message.get_data().max_ids.unwrap();
                println!(
                    "{color_bright_green}Memory size: {}{style_reset}",
                    memory_size
                );
                println!(
                    "{color_bright_green}Max Ids for Shard: {:?}{style_reset}",
                    max_ids_info
                );
                self.save_shard_in_manager(memory_size, node_port.to_string(), max_ids_info);
                true
            }
            MessageType::MemoryUpdate => {
                let memory_size = response_message.get_data().payload.unwrap();
                let max_ids_info = response_message.get_data().max_ids.unwrap();
                println!(
                    "{color_bright_green}Shard {} updated its memory size to {}{style_reset}",
                    node_port, memory_size
                );
                println!(
                    "{color_bright_green}Max Ids for Shard: {:?}{style_reset}",
                    max_ids_info
                );
                self.update_shard_in_manager(memory_size, node_port.to_string(), max_ids_info);
                true
            }
            _ => {
                println!(
                    "{color_red}Shard {} denied the connection{style_reset}",
                    node_port
                );
                false
            }
        }
    }

    /// Adds a shard to the ShardManager with the given memory size and shard id.
    fn save_shard_in_manager(&mut self, memory_size: f64, shard_id: String, max_ids: TablesIdInfo) {
        let mut shard_manager = self.shard_manager.as_ref().clone();
        shard_manager.add_shard(memory_size, shard_id.clone());
        shard_manager.save_max_ids_for_shard(shard_id.clone(), max_ids);
        println!(
            "{color_bright_green}Shard {} added to ShardManager{style_reset}",
            shard_id
        );
        println!("Shard Manager: {:?}", shard_manager);
    }

    /// Updates the shard in the ShardManager with the given memory size and shard id.
    fn update_shard_in_manager(
        &mut self,
        memory_size: f64,
        shard_id: String,
        max_ids: TablesIdInfo,
    ) {
        let mut shard_manager = self.shard_manager.as_ref().clone();
        shard_manager.update_shard_memory(memory_size, shard_id.clone());
        shard_manager.save_max_ids_for_shard(shard_id.clone(), max_ids);
        println!(
            "{color_bright_green}Shard {} updated in ShardManager{style_reset}",
            shard_id
        );
        println!("Shard Manager: {:?}", shard_manager);
    }

    /// Establishes a health connection with the node with the given ip and port, returning a Channel.
    fn get_shard_channel(node_ip: &str, node_port: &str) -> Result<Channel, io::Error> {
        let port = node_port.parse::<u64>().unwrap() + 1000;
        match TcpStream::connect(format!("{}:{}", node_ip, port)) {
            Ok(stream) => {
                println!(
                    "{color_bright_green}Health connection established with {}:{}{style_reset}",
                    node_ip, port
                );
                Ok(Channel {
                    stream: Arc::new(Mutex::new(stream)),
                })
            }
            Err(e) => {
                println!(
                    "{color_red}Error establishing health connection with {}:{}. Error: {:?}{style_reset}",
                    node_ip, port, e
                );
                Err(e)
            }
        }
    }

    /// Function that receives a query and checks for shards with corresponding data.
    /// If the query is an INSERT query, it will return the specific shard that the query should be sent to.
    /// If the query is not an INSERT query, it will return all shards.
    /// The second return value is a boolean that indicates if the shards need to update their memory after the query is executed. This will be true if the query affects the memory state of the system.
    /// Returns the query formatted if needed (if there's a 'WHERE ID=' clause, offset might need to be removed)
    fn get_data_needed_from(&mut self, query: &str) -> (Vec<String>, bool, String) {
        if let Some(id) = get_id_if_exists(query) {
            println!("ID found in query: {}", id);
            return self.get_specific_shard_with(id, query);
        }

        println!("ID NOT FOUND in query.");
        if query_is_insert(query) {
            println!("Query is INSERT");
            let shard = self.shard_manager.peek().unwrap();
            (vec![shard.clone()], true, query.to_string())
        } else {
            // Return all shards
            (
                self.shards.lock().unwrap().keys().cloned().collect(),
                query_affects_memory_state(query),
                query.to_string(),
            )
        }
    }

    fn get_specific_shard_with(&mut self, mut id: i64, query: &str) -> (Vec<String>, bool, String) {
        let table_name = match get_table_name_from_query(query) {
            Some(table_name) => table_name,
            None => {
                return (
                    self.shards.lock().unwrap().keys().cloned().collect(),
                    query_affects_memory_state(query),
                    query.to_string(),
                );
            }
        };
        println!("Table name: {}", table_name);
        for shard_id in self.shards.lock().unwrap().keys() {
            let max_id = match self
                .shard_manager
                .get_max_ids_for_shard_table(shard_id, &table_name)
            {
                Some(max_id) => max_id,
                None => continue,
            };
            if id > max_id {
                id -= max_id;
            } else {
                let formatted_query = format_query_with_new_id(query, id);
                return (
                    vec![shard_id.clone()],
                    query_affects_memory_state(query),
                    formatted_query,
                );
            }
        }

        println!("ID not found in any shard");
        return (
            self.shards.lock().unwrap().keys().cloned().collect(),
            query_affects_memory_state(query),
            query.to_string(),
        );
    }

    fn format_response(&self, shards_responses: IndexMap<String, Vec<Row>>, query: &str) -> String {
        let table_name = match get_table_name_from_query(query) {
            Some(table_name) => table_name,
            None => {
                eprintln!("Failed to get table name from query");
                return String::new();
            }
        };

        let mut rows_offset: Vec<(Vec<Row>, i64)> = Vec::new();
        let mut last_offset: i64 = 0;
        for (shard_id, rows) in shards_responses {
            let offset = match self
                .shard_manager
                .get_max_ids_for_shard_table(&shard_id, &table_name)
            {
                Some(offset) => offset,
                None => {
                    eprintln!("Failed to get offset for shard");
                    return String::new();
                }
            };
            rows_offset.push((rows, last_offset));
            last_offset = offset;
        }

        format_rows_with_offset(rows_offset)
    }
}

impl NodeRole for Router {
    fn send_query(&mut self, received_query: &str) -> Option<String> {
        println!("Router send_query called with query: {:?}", received_query);

        let (shards, is_insert, query) = self.get_data_needed_from(received_query);

        println!(
            "Shards: {:?}, is_insert: {}, query: {}",
            shards, is_insert, query
        );

        if shards.len() == 0 {
            eprintln!("No shards found for the query");
            return None;
        }

        let mut shards_responses: IndexMap<String, Vec<Row>> = IndexMap::new();
        let mut rows = Vec::new();
        for shard_id in shards {
            let shard_response = self.send_query_to_shard(shard_id.clone(), &query, is_insert);
            if !shard_response.is_empty() {
                shards_responses.insert(shard_id, shard_response.clone());
                rows.extend(shard_response);
            }
        }

        let response;
        if query_is_select(&query) && shards_responses.len() > 0 {
            println!("Query is SELECT and shards_responses is not empty");
            response = self.format_response(shards_responses, &query);
        } else {
            println!(
                "Query is SELECT: {}, shards_responses is empty: {}",
                query_is_insert(&query),
                shards_responses.is_empty()
            );
            response = rows.convert_to_string();
        }

        print_query_response(response.clone());
        Some(response)
    }
}

// Communication with shards
impl Router {
    fn get_stream(&self, shard_id: &str) -> Option<Arc<Mutex<TcpStream>>> {
        let comm_channels = match self.comm_channels.read() {
            Ok(comm_channels) => comm_channels,
            Err(_) => {
                eprintln!("Failed to get comm channels");
                return None;
            }
        };

        let shard_comm_channel = match comm_channels.get(&shard_id.to_string()) {
            Some(shard_comm_channel) => shard_comm_channel,
            None => {
                eprintln!("Failed to get comm channel for shard {}", shard_id);
                return None;
            }
        };

        Some(shard_comm_channel.stream.clone())
    }

    fn init_message_exchange(
        &mut self,
        message: Message,
        writable_stream: &mut MutexGuard<TcpStream>,
        shard_id: String,
    ) -> bool {
        writable_stream
            .write(message.to_string().as_bytes())
            .unwrap();
        let mut response: [u8; 1024] = [0; 1024];

        // Read and handle message
        writable_stream.read(&mut response).unwrap();
        let response_string = String::from_utf8_lossy(&response);
        let response_message = match Message::from_string(&response_string) {
            Ok(message) => message,
            Err(_) => {
                eprintln!("Failed to parse message from shard");
                // TODO-SHARD: handle this situation, should this try again? What happens if we can't update the shard's memory in the shard_manager?
                return false;
            }
        };

        self.handle_response(response_message, shard_id.as_str())
    }

    /// Function that sends a message to the shard asking for a memory update. This must be called each time an insertion query is sent, and may be used to update the shard's memory size in the ShardManager in other circumstances.
    fn ask_for_memory_update(&mut self, shard_id: String) {
        let stream = match self.get_stream(shard_id.as_str()) {
            Some(stream) => stream,
            None => {
                eprintln!("Failed to get stream for shard {}", shard_id);
                return;
            }
        };

        let mut writable_stream = match stream.as_ref().try_lock() {
            Ok(writable_stream) => writable_stream,
            Err(_) => {
                eprintln!("Failed to get writable stream for shard {}", shard_id);
                return;
            }
        };

        // Write message
        let message = Message::new_ask_memory_update();
        self.init_message_exchange(message, &mut writable_stream, shard_id);
    }

    fn send_query_to_shard(&mut self, shard_id: String, query: &str, update: bool) -> Vec<Row> {
        if let Some(shard) = self.clone().shards.lock().unwrap().get_mut(&shard_id) {
            let rows = match shard.query(query, &[]) {
                Ok(rows) => rows,
                Err(e) => {
                    eprintln!("Failed to send the query to the shard: {:?}", e);
                    return Vec::new();
                }
            };

            if update {
                self.ask_for_memory_update(shard_id);
            }

            return rows;
        } else {
            eprintln!("Shard {:?} not found", shard_id);
            return Vec::new();
        }
    }
}
