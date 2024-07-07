#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
use postgres::{Client, NoTls};
extern crate users;
use users::get_current_username;

// This should be behavior with trait, not enum
enum NodeType {
    Router,
    Shard,
}

// It would be wize to implement this with a Singleton
struct DistributionHandler {
    // This should hold the connection to each one of the shards
    client: Client,
    // This should be a behavior interface from a trait, not enum
    nodeType: NodeType,
}

// TODO-SHARD: This needs to be mapped into C to be able to initialize it from startup.c
impl DistributionHandler {
    fn new(nodeType: NodeType, port: &str) -> Self {
        DistributionHandler {
            client: DistributionHandler::connect(port),
            nodeType: nodeType,
        }
    }
    
    fn connect(port: &str) -> Client {
        // Connect to the database
        // get username dynamically
        let username = match get_current_username() {
            Some(username) => username.to_string_lossy().to_string(),
            None => panic!("Failed to get current username"),
        };
        println!("Username found: {:?}", username);
        
        match Client::connect(format!("host=127.0.0.1 port={} user={} dbname=template1", port, username).as_str(), NoTls) {
            Ok(client) => client,
            Err(e) => {
                eprintln!("Failed to connect to the database: {:?}", e);
                panic!("Failed to connect to the database");
            }
        }
    }

    // TODO_SHARD: Should this use pgwire to send the query to the shard(s)?
    fn handle_query(&mut self, query: &str) -> Result<(), Box<dyn std::error::Error>> {
        let rows = self.client.query(query, &[])?;
        println!("{:?}", rows);
        Ok(())
    }
}