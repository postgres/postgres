use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct NodeConfig {
    pub nodes: Vec<Node>,
}

#[derive(Debug, Deserialize)]
pub struct Node {
    pub ip: String,
    pub port: String,
    pub name: String,
}