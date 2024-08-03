// tests/integration_test.rs

use core::panic;
use postgres::{Client, NoTls};
use sharding::node::node::{get_node_instance, init_node_instance, NodeType};
use std::{
    io::Write,
    process::{Command, Stdio},
};
use users::get_current_username;

fn setup_connection(host: &str, port: &str, db_name: &str) -> Client {
    let username = match get_current_username() {
        Some(username) => username.to_string_lossy().to_string(),
        None => panic!("Failed to get current username"),
    };

    match Client::connect(
        format!(
            "host={} port={} user={} dbname={}",
            host, port, username, db_name
        )
        .as_str(),
        NoTls,
    ) {
        Ok(client) => {
            println!("Connected to host: {}, port: {}", host, port);
            client},
        Err(e) => {
            panic!("Failed to connect to the database: {:?}", e);
        }
    }
}

#[test]
fn test_nodes_initialize_empty() {
    create_and_init_cluster(b"test-shard\n", "s");
    create_and_init_cluster(b"test-router\n", "r");

    let mut router_connection: Client = setup_connection("localhost", "5433", "template1");
    let mut shard_connection: Client = setup_connection("localhost", "5434", "template1");

    // Count user tables, excluding system tables
    let row = router_connection
        .query_one(
            "SELECT COUNT(*) FROM pg_catalog.pg_tables WHERE schemaname = 'public'",
            &[],
        )
        .unwrap();
    let count: i64 = row.get(0);
    assert_eq!(count, 0);

    let row = shard_connection
        .query_one(
            "SELECT COUNT(*) FROM pg_catalog.pg_tables WHERE schemaname = 'public'",
            &[],
        )
        .unwrap();
    let count: i64 = row.get(0);
    assert_eq!(count, 0);

    stop_cluster(b"test-router\n");
    stop_cluster(b"test-shard\n");
}

#[test]
fn test_create_table() {
    create_and_init_cluster(b"test-shard1\n", "s");

    let mut shard_connection: Client = setup_connection("localhost", "5433", "template1");

    // Initialize and get the router
    init_node_instance(
        NodeType::Router,
        "5434".as_ptr() as *const i8,
        "src/node/config.yaml\0".as_ptr() as *const i8,
    );
    let router = get_node_instance();

    // Create a table on the router
    router.send_query("CREATE TABLE test_table (id INT PRIMARY KEY);");

    let mut router_connection: Client = setup_connection("localhost", "5434", "template1");

    // Count user tables in the router, excluding system tables. Should be zero.
    let row = router_connection
        .query_one(
            "SELECT COUNT(*) FROM pg_catalog.pg_tables WHERE schemaname = 'public'",
            &[],
        )
        .unwrap();
    let count: i64 = row.get(0);
    assert_eq!(count, 0);

    // Count user tables in the shard, excluding system tables. Should be one.
    let row = shard_connection
        .query_one(
            "SELECT COUNT(*) FROM pg_catalog.pg_tables WHERE schemaname = 'public'",
            &[],
        )
        .unwrap();
    let count: i64 = row.get(0);
    assert_eq!(count, 1);

    stop_cluster(b"test-shard\n");
    stop_cluster(b"test-router\n");
}

// Utility functions

fn create_and_init_cluster(node_name: &[u8], node_type: &str) {
    create_cluster_dir(node_name);
    init_cluster(node_name, node_type);
}

fn create_cluster_dir(node_name: &[u8]) {
    let mut create_cluster = Command::new("./create-cluster-dir.sh")
        .current_dir("..")
        .stdin(Stdio::piped())
        .spawn()
        .expect("failed to create cluster");

    {
        let stdin = create_cluster.stdin.as_mut().expect("failed to open stdin");
        stdin
            .write_all(node_name)
            .expect("failed to write to stdin");
    }

    let create_cluster_status = create_cluster
        .wait()
        .expect("failed to wait on create-cluster-dir.sh");
    if !create_cluster_status.success() {
        panic!("create-cluster-dir.sh failed");
    }
}

fn init_cluster(node_name: &[u8], node_type: &str) {
    let mut init_cluster = Command::new("./init-server.sh")
        .current_dir("..")
        .arg("start")
        .arg(node_type)
        .stdin(Stdio::piped())
        .spawn()
        .expect("failed to start cluster");

    {
        let stdin = init_cluster.stdin.as_mut().expect("failed to open stdin");
        stdin
            .write_all(node_name)
            .expect("failed to write to stdin");
    }

    let init_cluster_status = init_cluster
        .wait()
        .expect("failed to wait on init-server.sh");
    if !init_cluster_status.success() {
        panic!("init-server.sh failed");
    }
}

fn stop_cluster(node_name: &[u8]) {
    let mut stop_cluster = Command::new("./server-down.sh")
        .current_dir("..")
        .stdin(Stdio::piped())
        .spawn()
        .expect("failed to stop cluster");

    {
        let stdin = stop_cluster.stdin.as_mut().expect("failed to open stdin");
        stdin
            .write_all(node_name)
            .expect("failed to write to stdin");
    }
}
