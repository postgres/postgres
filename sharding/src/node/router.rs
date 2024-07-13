use postgres::{Client, NoTls};
extern crate users;
use rust_decimal::Decimal;
use users::get_current_username;
use std::fs;
use super::node::*;

/// This struct represents the Router node in the distributed system. It has the responsibility of routing the queries to the appropriate shard or shards.
#[repr(C)]
pub struct Router<'a> {
    clients: Vec<Client>,
    port:&'a str,
    // TODO-SHARD: add hash table for routing
}

impl<'a> Router<'a> {
    /// Creates a new Router node with the given port
    pub fn new(port: &'a str) -> Self {

        // read from 'ports.txt' to get the ports
        let contents = fs::read_to_string("/Users/aldanarastrelli/Documents/Aldana/distributed-postgres/sharding/src/node/ports.txt")
        .expect("Should have been able to read the file");
        let ports: Vec<&str> = contents.split("\n").collect();
        let mut clients = Vec::new();
        for client_port in ports {
            if client_port == port {
                continue
            }
            match Router::connect(client_port) {
                Ok(client) => clients.push(client),
                Err(e) => {}, // Do something here
            }
        }
        if clients.is_empty() {
            eprint!("Failed to connect to any of the nodes");
        };

        Router {
            clients: clients,
            port
        }
    }

    fn connect(port: &str) -> Result<Client, postgres::Error> {
        // get username dynamically
        let username = match get_current_username() {
            Some(username) => username.to_string_lossy().to_string(),
            None => panic!("Failed to get current username"),
        };
        println!("Username found: {:?}", username);
        
        // TODO-SHARD host should be dynamic or from configuration file
        match Client::connect(format!("host=127.0.0.1 port={} user={} dbname=template1", port, username).as_str(), NoTls) {
            Ok(client) => Ok(client),
            Err(e) => {
                Err(e)
            }
        }
    }
}

impl<'a> NodeRole for Router<'a> {
    fn send_query(&mut self, query: &str) -> bool {
        println!("Router send_query called with query: {:?}", query);
        // TODO-SHARD: implement the routing logic
        
        // here

        // send the query to the first shard for now. When routing logic is added, the change needed is:
        // get the client from the routing logic, delete the first line down here, leave the rest to execute for the found client.
        // If the query needs to be sent to multiple shards, then the logic should be changed to send the query to all the shards (found in the routing logic or all the shards in general).
        let client = &mut self.clients[0];
        let rows = match client.query(query, &[]) {
            Ok(rows) => rows,
            Err(e) => {
                eprintln!("Failed to send the query to the shard: {:?}", e);
                return false;
            }
        };

        for row in rows {
            let id: i32 = row.get(0);
            let name: &str = row.get(1);
            let position: &str = row.get(2);
            let salary: Decimal = row.get(3);
            println!(
                "QUERY RESULT: id: {}, name: {}, position: {}, salary: {}",
                id, name, position, salary
            );
        }
        true
    }
}
