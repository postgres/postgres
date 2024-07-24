use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct ServerConfig {
    pub servers: Vec<Server>,
}

#[derive(Debug, Deserialize)]
pub struct Server {
    pub ip: String,
    pub port: String,
    pub name: String,
}