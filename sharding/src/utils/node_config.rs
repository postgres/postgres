use raft::node_config::*;
use serde::Deserialize;
use std::fs;

#[derive(Debug, Deserialize)]
pub struct NodesConfig {
    pub nodes: Vec<Node>,
}

#[derive(Debug, Deserialize)]
pub struct Node {
    pub ip: String,
    pub port: String,
    pub name: String,
}

#[derive(Debug, Deserialize)]
pub struct LocalNode {
    pub unavailable_memory_perc: f64,
}

pub fn get_nodes_config(config_file_path: Option<&str>) -> NodesConfig {
    // read from 'config.yaml' to get the NodeConfig
    let config_file_path = match config_file_path {
        Some(path) => path,
        None => "../../../sharding/src/node/config/nodes_config.yaml",
    };

    let config_content =
        fs::read_to_string(config_file_path).expect("Should have been able to read the file");

    serde_yaml::from_str(&config_content).expect("Should have been able to parse the YAML")
}

pub fn get_nodes_config_raft(config_file_path: Option<&str>) -> raft::node_config::NodesConfig {
    // read from 'config.yaml' to get the NodeConfig from raft
    let config_file_path = match config_file_path {
        Some(path) => path,
        None => "../../../sharding/src/node/config/nodes_config.yaml",
    };

    let config_content =
        fs::read_to_string(config_file_path).expect("Should have been able to read the file");

    serde_yaml::from_str(&config_content).expect("Should have been able to parse the YAML")
}

pub fn get_memory_config() -> LocalNode {
    let config_content = fs::read_to_string("../../../sharding/src/node/config/memory_config.yaml")
        .expect("Should have been able to read the file");

    serde_yaml::from_str(&config_content).expect("Should have been able to parse the YAML")
}
