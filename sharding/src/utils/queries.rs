use postgres::{Column, Row};
use rust_decimal::Decimal;

use super::common::ConvertToString;

struct QueryTypes;

impl QueryTypes {
    const INSERT: &'static str = "INSERT";
    const DELETE: &'static str = "DELETE";
    const DROP: &'static str = "DROP";
    const UPDATE: &'static str = "UPDATE";
    const CREATE: &'static str = "CREATE";
}

pub fn query_is_insert(query: &str) -> bool {
    query_is(query, QueryTypes::INSERT)
}

pub fn query_is_select(query: &str) -> bool {
    query_is(query, "SELECT")
}

fn query_is(query: &str, query_type: &str) -> bool {
    query.to_uppercase().starts_with(query_type)
}

pub fn query_affects_memory_state(query: &str) -> bool {
    query_is(query, QueryTypes::INSERT)
        || query_is(query, QueryTypes::DELETE)
        || query_is(query, QueryTypes::DROP)
        || query_is(query, QueryTypes::UPDATE)
        || query_is(query, QueryTypes::CREATE)
}

/// Gets the name of the table from a query, whenever the query has a "FROM <tablename>" clause.
pub fn get_table_name_from_query(query: &str) -> Option<String> {
    if !query_is(query, "SELECT") {
        return None;
    }
    let query_aux = query.to_uppercase();
    let from_index = query_aux.find("FROM")?;
    let mut table_name = query[from_index + 5..].split_whitespace().next()?;
    // delete the char ";" if it exists in the table name
    if table_name.ends_with(';') {
        table_name = &table_name[..table_name.len() - 1];
    }
    Some(table_name.to_string())
}

// ************** ToString TRAIT **************

trait ConvertToStringOffset {
    fn convert_to_string_with_offset(&self, offset: i64) -> String;
}

impl ConvertToString for Row {
    fn convert_to_string(&self) -> String {
        let mut result = String::new();
        // If is empty, return empty string
        if self.is_empty() {
            return result;
        }
        for (i, _) in self.columns().iter().enumerate() {
            // Try to get the value as a String, If it fails, try to get it as an i32. Same for f64 and Decimal
            let formatted_value = match self.try_get::<usize, String>(i) {
                Ok(v) => format!("{}", v),
                Err(_) => match self.try_get::<usize, i32>(i) {
                    Ok(v) => format!("{}", v),
                    Err(_) => match self.try_get::<usize, f64>(i) {
                        Ok(v) => format!("{}", v),
                        Err(_) => match self.try_get::<usize, Decimal>(i) {
                            Ok(v) => format!("{}", v),
                            Err(_) => String::new(),
                        },
                    },
                },
            };

            result.push_str(&formatted_value);
            result.push_str(" | ");
        }
        result
    }
}

impl ConvertToStringOffset for Row {
    fn convert_to_string_with_offset(&self, offset: i64) -> String {
        let mut result = String::new();
        // If is empty, return empty string
        if self.is_empty() {
            return result;
        }
        
        for (i, _) in self.columns().iter().enumerate() {
            let is_id = self.columns()[i].name().to_string() == "id";

            // Try to get the value as a String, If it fails, try to get it as an i32. Same for f64 and Decimal
            let formatted_value = match self.try_get::<usize, String>(i) {
                Ok(v) => format!("{}", v),
                Err(_) => match self.try_get::<usize, i32>(i) {
                    Ok(v) => format!("{}", v),
                    Err(_) => match self.try_get::<usize, f64>(i) {
                        Ok(v) => format!("{}", v),
                        Err(_) => match self.try_get::<usize, Decimal>(i) {
                            Ok(v) => format!("{}", v),
                            Err(_) => String::new(),
                        },
                    },
                },
            };

            if is_id {
                // If the column name is 'id', sum the offset to the value
                result.push_str(&format!("{}", formatted_value.parse::<i64>().unwrap() + offset));
            } else {
            result.push_str(&formatted_value);
            }
            result.push_str(" | ");
        }

        result
    }
}

impl ConvertToString for Vec<Row> {
    fn convert_to_string(&self) -> String {
        let mut result = String::new();
        // If is empty, return empty string
        if self.is_empty() {
            return result;
        }

        // Get column names and add them to the result, separated by a pipe
        let columns = self[0].columns();
        let columns_names = get_column_names(columns);
        result.push_str(&columns_names);
        result.push('\n');

        for row in self {
            result.push_str(&row.convert_to_string());
            result.push('\n');
        }
        result
    }
}

fn get_column_names(columns: &[Column]) -> String {
    let mut result = String::new();
    for column in columns {
        result.push_str(&column.name().to_string());
        result.push_str(" | ");
    }
    result
}

pub fn print_rows(rows: Vec<Row>) {
    let response = rows.convert_to_string();
    print_query_response(response);
}

pub fn print_query_response(reponse: String) {
    // Split by \n and print each line
    for line in reponse.split('\0') {
        if line.is_empty() {
            continue;
        }
        println!("{}", line);
    }
}

pub fn format_rows_with_offset(rows_offset: Vec<(Vec<Row>, i64)>) -> String {
    let mut result = String::new();

    // Get column names and add them to the result, separated by a pipe
    let columns = rows_offset[0].0[0].columns();
    let columns_names = get_column_names(columns);
    result.push_str(&columns_names);
    result.push('\0');

    // For each Row, convert it to string. Get the id value and add the offset to it
    for (rows, offset) in rows_offset {
        for row in rows {
            result.push_str(&row.convert_to_string_with_offset(offset));
            result.push('\0');
        }
    }

    result
}

#[cfg(test)]

mod tests {
    use super::*;

    #[test]
    fn test_query_is_insert() {
        assert!(query_is_insert("INSERT INTO employees (id, name) VALUES (1, 'Alice')"));
        assert!(!query_is_insert("SELECT * FROM employees"));
    }

    #[test]
    fn test_query_affects_memory_state() {
        assert!(query_affects_memory_state("INSERT INTO employees (id, name) VALUES (1, 'Alice')"));
        assert!(query_affects_memory_state("DELETE FROM employees WHERE id = 1"));
        assert!(query_affects_memory_state("DROP TABLE employees"));
        assert!(query_affects_memory_state("UPDATE employees SET name = 'Alice' WHERE id = 1"));
        assert!(query_affects_memory_state("CREATE TABLE employees (id INT, name TEXT)"));
        assert!(!query_affects_memory_state("SELECT * FROM employees"));
    }

    #[test]
    fn test_query_is() {
        assert!(query_is("INSERT INTO employees (id, name) VALUES (1, 'Alice')", QueryTypes::INSERT));
        assert!(!query_is("SELECT * FROM employees", QueryTypes::INSERT));
    }

    #[test]
    fn test_get_table_name_from_query() {
        assert_eq!(get_table_name_from_query("SELECT * FROM employees"), Some("employees".to_string()));
        assert_eq!(get_table_name_from_query("INSERT INTO employees (name, position, salary) VALUES ('Alice Johnson', 'Software Engineer', 85000);"), None);
    }
}