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
    // Call get_table_name_behind_keyword with the keywords "FROM", "UPDATE", INTO and TABLE
    let table_name = match get_table_name_behind_keyword(query, "FROM".to_string())
        .or_else(|| get_table_name_behind_keyword(query, "UPDATE".to_string()))
        .or_else(|| get_table_name_behind_keyword(query, "INTO".to_string()))
        .or_else(|| get_table_name_behind_keyword(query, "TABLE".to_string())) {
        Some(table_name) => table_name,
        None => return None,
        };

    let mut table_name = table_name.as_str();
    // delete the char ";" if it exists in the table name
    if table_name.ends_with(';') {
        table_name = &table_name[..table_name.len() - 1];
    }
    Some(table_name.to_string())
}

fn get_table_name_behind_keyword(query: &str, keyword: String) -> Option<String> {
    let query_aux = query.to_uppercase();
    let from_index = query_aux.find(&keyword)?;
    let right_side_query = &query[from_index + keyword.len()..];
    // Split by spaces and get the first element
    let table_name = right_side_query.split_whitespace().next()?;
    Some(table_name.to_string())
}

fn get_id_index(query: &str) -> Option<usize> {
    let query_aux = query.to_uppercase();
    let query_substring1: &str = "WHERE ID = "; // Both spaces
    let query_substring2: &str = "WHERE ID ="; // No right space
    let query_substring3: &str = "WHERE ID= "; // No left space
    let query_substring4: &str = "WHERE ID="; // No spaces
    let offset1 = query_substring1.len();
    let offset2 = query_substring2.len();
    let offset3 = query_substring3.len();
    let offset4 = query_substring4.len();

    let index1 = query_aux.find(query_substring1);
    let index2 = query_aux.find(query_substring2);
    let index3 = query_aux.find(query_substring3);
    let index4 = query_aux.find(query_substring4);

    if index1.is_some() {
        return Some(index1.unwrap() + offset1);
    } else if index2.is_some() {
        return Some(index2.unwrap() + offset2);
    } else if index3.is_some() {
        return Some(index3.unwrap() + offset3);
    } else if index4.is_some() {
        return Some(index4.unwrap() + offset4);
    } else {
        return None;
    }
}

fn get_trimmed_id(query: &str, from: usize) -> String {
    let mut id = query[from..].trim();
    if id.ends_with(';') {
        id = &id[..id.len() - 1];
    }
    return id.to_string();
}

pub fn get_id_if_exists(query: &str) -> Option<i64> {
    let id_index = get_id_index(query)?;
    let id = get_trimmed_id(query, id_index);
    Some(id.parse::<i64>().unwrap())
}

/// Finds the 'WHERE ID=' clause, and changes the value of the id to the new_id
pub fn format_query_with_new_id(query: &str, new_id: i64) -> String {
    let id_index = get_id_index(query).unwrap();
    let id = get_trimmed_id(query, id_index);
    let id_len = id.len();

    let mut new_query = String::new();
    new_query.push_str(&query[..id_index]);
    new_query.push_str(&new_id.to_string());
    new_query.push_str(&query[id_index + id_len..]);
    new_query
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
    fn test_get_table_name_behind_keyword() {
        assert_eq!(get_table_name_behind_keyword("SELECT * FROM employees", "FROM".to_string()), Some("employees".to_string()));
        assert_eq!(get_table_name_behind_keyword("UPDATE employees SET name = 'Alice'", "UPDATE".to_string()), Some("employees".to_string()));
        assert_eq!(get_table_name_behind_keyword("INSERT INTO employees (name) VALUES ('Alice')", "INTO".to_string()), Some("employees".to_string()));
        assert_eq!(get_table_name_behind_keyword("CREATE TABLE employees (id INT, name TEXT)", "TABLE".to_string()), Some("employees".to_string()));
    }

    #[test]
    fn test_get_table_name_from_query() {
        assert_eq!(get_table_name_from_query("SELECT * FROM employees"), Some("employees".to_string()));
        assert_eq!(get_table_name_from_query("INSERT INTO employees (name, position, salary) VALUES ('Alice Johnson', 'Software Engineer', 85000);"), Some("employees".to_string()));
    }

    #[test]
    fn test_get_id_index() {
        // Both spaces left and right
        assert_eq!(get_id_index("SELECT * FROM employees WHERE id = 1;"), Some(35));
        assert_eq!(get_id_index("SELECT * FROM table_name WHERE id = 3;"), Some(36));
        // No space right
        assert_eq!(get_id_index("SELECT * FROM employees WHERE id =1;"), Some(34));
        assert_eq!(get_id_index("SELECT * FROM table_name WHERE id =3;"), Some(35));
        // No space left
        assert_eq!(get_id_index("SELECT * FROM employees WHERE id= 1;"), Some(34));
        assert_eq!(get_id_index("SELECT * FROM table_name WHERE id= 3;"), Some(35));
        // No spaces
        assert_eq!(get_id_index("SELECT * FROM employees WHERE id=1;"), Some(33));
        assert_eq!(get_id_index("SELECT * FROM table_name WHERE id=3;"), Some(34));
    }

    #[test]
    fn test_get_trimmed_id() {
        // Both spaces left and right
        assert_eq!(get_trimmed_id("SELECT * FROM employees WHERE id = 1;", 35), "1");
        assert_eq!(get_trimmed_id("SELECT * FROM table_name WHERE id = 3;", 36), "3");
        // No space right
        assert_eq!(get_trimmed_id("SELECT * FROM employees WHERE id =1;", 34), "1");
        assert_eq!(get_trimmed_id("SELECT * FROM table_name WHERE id =3;", 35), "3");
        // No space left
        assert_eq!(get_trimmed_id("SELECT * FROM employees WHERE id= 1;", 34), "1");
        assert_eq!(get_trimmed_id("SELECT * FROM table_name WHERE id= 3;", 35), "3");
        // No spaces
        assert_eq!(get_trimmed_id("SELECT * FROM employees WHERE id=1;", 33), "1");
        assert_eq!(get_trimmed_id("SELECT * FROM table_name WHERE id=3;", 34), "3");
    }

    #[test]
    fn test_get_id_if_exists() {
        // Both spaces left and right
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id = 1;"), Some(1));
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id = 3;"), Some(3));
        // No space right
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id =1;"), Some(1));
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id =3;"), Some(3));
        // No space left
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id= 1;"), Some(1));
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id= 3;"), Some(3));
        // No spaces
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id=1;"), Some(1));
        assert_eq!(get_id_if_exists("SELECT * FROM employees WHERE id=3;"), Some(3));
    }

    #[test]
    fn test_format_query_with_new_id() {
        // Both spaces left and right
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id = 1;", 3), "SELECT * FROM employees WHERE id = 3;");
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id = 3;", 1), "SELECT * FROM employees WHERE id = 1;");
        // No space right
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id =1;", 3), "SELECT * FROM employees WHERE id =3;");
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id =3;", 1), "SELECT * FROM employees WHERE id =1;");
        // No space left
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id= 1;", 3), "SELECT * FROM employees WHERE id= 3;");
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id= 3;", 1), "SELECT * FROM employees WHERE id= 1;");
        // No spaces
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id=1;", 3), "SELECT * FROM employees WHERE id=3;");
        assert_eq!(format_query_with_new_id("SELECT * FROM employees WHERE id=3;", 1), "SELECT * FROM employees WHERE id=1;");
    }

}