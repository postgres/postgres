use postgres::Row;
use rust_decimal::Decimal;

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

// ************** ToString TRAIT **************

pub trait ConvertToString {
    fn convert_to_string(&self) -> String;
}

impl ConvertToString for Row {
    fn convert_to_string(&self) -> String {
        let mut result = String::new();
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

impl ConvertToString for Vec<Row> {
    fn convert_to_string(&self) -> String {
        let mut result = String::new();

        // Get column names and add them to the result, separated by a pipe
        let columns = self[0].columns();
        for column in columns {
            result.push_str(&column.name().to_string());
            result.push_str(" | ");
        }

        result.push('\n');

        for row in self {
            result.push_str(&row.convert_to_string());
            result.push('\n');
        }
        result
    }
}
