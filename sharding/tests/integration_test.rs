// tests/integration_test.rs

use core::panic;
use postgres::{Client, NoTls};
use std::{
    io::Write,
    process::{Command, Stdio},
};
use users::get_current_username;

fn setup_db() -> Client {
    let host = "localhost";
    let port = "5433";
    let db_name = "template1";
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
        Ok(client) => client,
        Err(e) => {
            panic!("Failed to connect to the database: {:?}", e);
        }
    }
}

#[test]
fn test_inserts_and_selects() {
    // Run your scripts to set up and start the cluster
    create_cluster_dir(b"test-router\n");
    create_cluster_dir(b"test-shard\n");
    init_cluster(b"test-router\n", "r");
    init_cluster(b"test-shard\n", "s");

    let mut client: Client = setup_db();

    // Count user tables, excluding system tables
    let row = client.query_one(
        "SELECT COUNT(*) FROM pg_catalog.pg_tables WHERE schemaname = 'public'",
        &[],
    ).unwrap();
    let count: i64 = row.get(0);
    assert_eq!(count, 0);

    stop_cluster(b"test-router\n");
    stop_cluster(b"test-shard\n");
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
