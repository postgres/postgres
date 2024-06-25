mod psql;

use psql::common_h_bindings::{PGresult};
use std::ffi::CString;

extern "C" {
    pub fn PSQLexec(query: *const libc::c_char) -> *mut PGresult;
}

extern "C" fn print_pgresult() {
    unsafe {
        let result: *mut PGresult =
            PSQLexec(CString::new("SELECT * FROM employees;").unwrap().as_ptr());
        println!("{:?}", (*result).ntups);
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    print_pgresult();

    
    // // Start the pgwire server on port 5432
    // let server = TcpListener::bind("127.0.0.1:5433")?;

    // // Handle incoming connections
    // for connection in server.incoming() {
    //     println!("Client connected from {:?}", connection.addr());
    //     let mut empty_array: [u8; 512] = [0; 512];
    //     let _ = connection.unwrap().read(&mut *empty_array);
    //     let mut file = OpenOptions::new()
    //         .read(true)
    //         .write(true)
    //         .create(true)
    //         .open("../../src/bin/psql/testing.txt");

    //     // Write the buffer to the file
    //     file.write_all(&mut *empty_array)?;

    //     // Flush the data to ensure it's written to disk
    //     file.flush()?;
    // }

    Ok(())
}
