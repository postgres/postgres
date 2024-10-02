use postgres::{Client as PostgresClient, NoTls, Row};
use std::{
    net::TcpStream,
    sync::{Arc, Mutex},
};
use users::get_current_username;

pub fn get_username_dinamically() -> String {
    match get_current_username() {
        Some(username) => username.to_string_lossy().to_string(),
        None => panic!("Failed to get current username"),
    }
}

/// Connects to the node with the given ip and port, returning a Client.
pub fn connect_to_node(ip: &str, port: &str) -> Result<PostgresClient, postgres::Error> {
    let username = get_username_dinamically();

    match PostgresClient::connect(
        format!(
            "host={} port={} user={} dbname=template1",
            ip, port, username
        )
        .as_str(),
        NoTls,
    ) {
        Ok(shard_client) => Ok(shard_client),
        Err(e) => Err(e),
    }
}

#[derive(Clone)]
pub struct Channel {
    pub stream: Arc<Mutex<TcpStream>>,
}

pub trait ConvertToString {
    fn convert_to_string(&self) -> String;
}

pub trait FromString {
    fn from_string(string: &str) -> Self;
}