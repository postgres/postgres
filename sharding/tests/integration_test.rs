// tests/integration_test.rs

use core::panic;
use postgres::{Client, NoTls};
use sharding::node::node::{get_node_instance, init_node_instance, NodeType};
use std::{
    io::Write,
    process::{Command, Stdio},
};
use users::get_current_username;

fn setup_connection(host: &str, port: &str, db_name: &str) -> Option<Client> {
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
        Ok(client) => Some(client),
        Err(e) => {
            panic!("Failed to connect to the database: {:?}", e);
        }
    }
}

#[test]
fn test_nodes_initialize_empty() {
    create_and_init_cluster(b"test-shard\n", "s", "localhost", "5433");
    create_and_init_cluster(b"test-router\n", "r", "localhost", "5434");

    let mut router_connection: Client = setup_connection("localhost", "5433", "template1").unwrap();
    let mut shard_connection: Client = setup_connection("localhost", "5434", "template1").unwrap();

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
    create_and_init_cluster(b"test-shard1\n", "s", "localhost", "5433");
    create_and_init_cluster(b"test-router1\n", "r", "localhost", "5434");

    let mut shard_connection: Client = setup_connection("localhost", "5433", "template1").unwrap();

    // Initialize and get the router
    init_node_instance(
        NodeType::Router,
        "5434\0".as_ptr() as *const i8,
        "src/node/router_config.yaml\0".as_ptr() as *const i8,
    );
    let router = get_node_instance();

    // Create a table on the router
    assert_eq!(true, router.send_query("DROP TABLE IF EXISTS test_table;"));
    assert_eq!(
        true,
        router.send_query("CREATE TABLE test_table (id INT PRIMARY KEY);")
    );

    // Count user tables in the shard, excluding system tables. Should be one.
    let row = shard_connection
        .query_one(
            "SELECT COUNT(*) FROM pg_catalog.pg_tables WHERE schemaname = 'public';",
            &[],
        )
        .unwrap();
    let count: i64 = row.get(0);
    assert_eq!(count, 1);

    stop_cluster(b"test-shard1\n");
    stop_cluster(b"test-router1\n");
}

#[test]
fn test_insert_into_table_select_and_delete() {
    create_and_init_cluster(b"test-shard2\n", "s", "localhost", "5433");
    create_and_init_cluster(b"test-router2\n", "r", "localhost", "5434");

    let mut shard_connection: Client = setup_connection("localhost", "5433", "template1").unwrap();

    // Initialize and get the router
    init_node_instance(
        NodeType::Router,
        "5434\0".as_ptr() as *const i8,
        "src/node/router_config.yaml\0".as_ptr() as *const i8,
    );
    let router = get_node_instance();

    // Create a table on the router
    assert_eq!(true, router.send_query("DROP TABLE IF EXISTS test_table;"));
    assert_eq!(
        true,
        router.send_query("CREATE TABLE test_table (id INT PRIMARY KEY);")
    );

    // Insert ten rows into the table
    for i in 0..10000 {
        assert_eq!(
            true,
            router.send_query(&format!("INSERT INTO test_table VALUES ({});", i))
        );
    }

    // Select all rows from the table using the shard connection
    let rows = shard_connection
        .query("SELECT * FROM test_table;", &[])
        .unwrap();
    assert_eq!(rows.len(), 10000);

    // Validate the data inserted in each row
    for (i, row) in rows.iter().enumerate() {
        let id: i32 = row.get(0);
        assert_eq!(id, i as i32);
    }

    // Delete half of the rows from the table using the router connection
    assert_eq!(
        true,
        router.send_query("DELETE FROM test_table WHERE id % 2 = 0;")
    );

    // Select all rows from the table using the shard connection
    let rows = shard_connection
        .query("SELECT * FROM test_table;", &[])
        .unwrap();
    assert_eq!(rows.len(), 5000);

    stop_cluster(b"test-shard2\n");
    stop_cluster(b"test-router2\n");
}

// Utility functions

fn create_and_init_cluster(node_name: &[u8], node_type: &str, ip: &str, port: &str) {
    create_cluster_dir(node_name);
    init_cluster(std::str::from_utf8(node_name).unwrap(), node_type);
    wait_for_postgres(ip, port);
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

fn init_cluster(node_name: &str, node_type: &str) {
    Command::new("./init-server.sh")
        .current_dir("..")
        .arg("start")
        .arg(node_type)
        .arg(node_name)
        .arg("&")
        .spawn()
        .expect("failed to start cluster");
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

fn wait_for_postgres(host: &str, port: &str) {
    let username = match get_current_username() {
        Some(username) => username.to_string_lossy().to_string(),
        None => panic!("Failed to get current username"),
    };

    let mut attempts = 0;
    loop {
        attempts += 1;
        if attempts > 30 {
            panic!("PostgreSQL server did not start in time");
        }

        match Client::connect(
            &format!(
                "host={} port={} user={} dbname=template1",
                host, port, username
            ),
            NoTls,
        ) {
            Ok(_) => break,
            Err(_) => std::thread::sleep(std::time::Duration::from_secs(1)),
        }
    }
}
