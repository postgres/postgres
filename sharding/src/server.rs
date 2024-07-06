use postgres::{Client, NoTls};

pub fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Do smth
    Ok(())
}

// pub fn handle_query(query: &str) -> Result<(), Box<dyn std::error::Error>> {
//     println!("handle_query from server.rs called");
//     /*
//     $ ./psql -h localhost -p 5432 -U ncontinanza -d postgres
//     postgres=# \c template1

//     template1=# \dt

//     template1=# CREATE TABLE employees (
//         id SERIAL PRIMARY KEY,
//         name VARCHAR(100),
//         position VARCHAR(50),
//         salary NUMERIC
//     );

//     INSERT INTO employees (name, position, salary) VALUES ('Alice Johnson', 'Software Engineer', 85000);
//     INSERT INTO employees (name, position, salary) VALUES ('Bob Smith', 'Project Manager', 95000);
//     INSERT INTO employees (name, position, salary) VALUES ('Carol White', 'Designer', 70000);
//     INSERT INTO employees (name, position, salary) VALUES ('David Brown', 'QA Tester', 65000);
//     INSERT INTO employees (name, position, salary) VALUES ('Eve Davis', 'HR Specialist', 60000);
//     */
//     let mut client = Client::connect("host=127.0.0.1 user=aldanarastrelli dbname=template1", NoTls).unwrap();

//     let rows = client.query(query, &[])?;

//     println!("{:?}", rows);

//     Ok(())
// }
