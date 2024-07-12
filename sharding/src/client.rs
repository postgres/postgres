use tokio::io::{self, AsyncBufReadExt, BufReader};
use std::net::TcpStream;
use std::error::Error;
use std::time::Duration;
use std::io::{Read, Write};
use inline_colorization::*;

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    println!("{color_blue}{style_bold}[DISTRIBUTED POSTGRESQL] Welcome to Distributed PostgreSQL!{style_reset}");
    let mut connection = TcpStream::connect("localhost:10000")?;
    println!("{color_green}[DISTRIBUTED POSTGRESQL] Connected to server at localhost:10000{style_reset}");

    let stdin = io::stdin();
    let mut reader = BufReader::new(stdin).lines();

    println!("Enter your SQL query: ");

    while let Some(line) = reader.next_line().await? {
        if line.trim().is_empty() {
            continue;
        }

        // SQL query to be sent
        let sql_query = line;

        let _ = connection.write_all(sql_query.as_bytes());
        println!("{color_green}Query sent: {}{style_reset}", sql_query);

        tokio::time::sleep(Duration::from_secs(1)).await;
        println!("[DISTRIBUTED POSTGRESQL] Enter your SQL query: ");
    }

    Ok(())
}
