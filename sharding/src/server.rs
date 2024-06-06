use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::fs::{self, OpenOptions};
use std::process::{Command, Stdio};
use std::str;

fn handle_client(mut stream: TcpStream, logfile: &mut std::fs::File) -> std::io::Result<()> {
    let mut buffer = [0; 512];
    let bytes_read = stream.read(&mut buffer)?;
    if bytes_read > 0 {
        let db_name = str::from_utf8(&buffer[..bytes_read]).unwrap_or("");
        if !db_name.trim().is_empty() {
            println!("{}", db_name);
        }
    }
    buffer = [0; 512]; // Clear the buffer

    // Read Query
    let bytes_read = stream.read(&mut buffer)?;
    if bytes_read > 0 {
        let query = str::from_utf8(&buffer[..bytes_read]).unwrap_or("");
        if !query.trim().is_empty() {
            writeln!(logfile, "{}", query).expect("Query writing failed");
        }
    }

    // Canonicalize the path to get the full path
    let psql_path = fs::canonicalize("/home/franco/distributed-postgres/src/bin/psql")?;
    let logfile_path = fs::canonicalize("logfile.txt")?;

    // Open the logfile for reading
    let mut input_logfile = std::fs::File::open(&logfile_path)?;
    let mut log_contents = String::new();
    input_logfile.read_to_string(&mut log_contents)?;

    // Execute the psql command with logfile as input
    let mut psql_command = Command::new(psql_path.join("psql"))
        .arg("-h")
        .arg("localhost")
        .arg("-p")
        .arg("5432")
        .arg("-U")
        .arg("franco")
        .arg("-d")
        .arg("postgres")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Failed to execute psql command");

    if let Some(mut stdin) = psql_command.stdin.take() {
        stdin.write_all(log_contents.as_bytes())?;
    }

    let output = psql_command.wait_with_output()?;
    stream.write_all(&output.stdout)?;
    stream.write_all(&output.stderr)?;

    Ok(())
}

fn main() -> std::io::Result<()> {
    let listener = TcpListener::bind("127.0.0.1:7878")?;
    println!("Server listening on port 7878");

    // Open or create the logfile
    let mut logfile = OpenOptions::new()
        .create(true)
        .append(true)
        .open("logfile.txt")?;

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                match handle_client(stream, &mut logfile) {
                    Ok(_) => {
                        println!("Client handled successfully");
                    }
                    Err(e) => {
                        eprintln!("Error handling client: {}", e);
                    }
                }
            }
            Err(e) => {
                eprintln!("Error accepting connection: {}", e);
            }
        }
    }
    Ok(())
}
