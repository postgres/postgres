use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use std::error::Error;
use std::thread::sleep;
use std::time::Duration;

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let mut connection = TcpStream::connect("localhost:10000").await?;

    // SQL query to be sent
    let sql_query = "SELECT * FROM employees;";

    connection.write_all(sql_query.as_bytes()).await?;
    println!("Query sent: {}", sql_query);

    let mut buffer = vec![0; 1024];

    tokio::time::sleep(Duration::from_secs(30)).await;
    Ok(())
}
