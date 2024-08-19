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
    pub memory_threshold: f64,
}

pub fn get_router_config(config_file_path: Option<&str>) -> NodesConfig {
    // read from 'config.yaml' to get the NodeConfig
    let config_file_path = match config_file_path {
        Some(path) => path,
        None => "../../../sharding/src/node/router_config.yaml",
    };

    let config_content =
        fs::read_to_string(config_file_path).expect("Should have been able to read the file");

    serde_yaml::from_str(&config_content).expect("Should have been able to parse the YAML")
}

pub fn get_shard_config() -> LocalNode {
    let config_content = fs::read_to_string("../../../sharding/src/node/shard_config.yaml")
        .expect("Should have been able to read the file");

    serde_yaml::from_str(&config_content).expect("Should have been able to parse the YAML")
}
