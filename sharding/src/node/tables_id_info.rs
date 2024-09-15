use indexmap::IndexMap;
use crate::utils::common::{ConvertToString, FromString};

pub type TablesIdInfo = IndexMap<String, i64>;

impl ConvertToString for TablesIdInfo {
    fn convert_to_string(&self) -> String {
        let mut result = Vec::new();
        for (key, value) in self.iter() {
            result.push(format!("{}:{}", key, value));
        }
        result.join(",")
    }
}

impl FromString for TablesIdInfo {
    fn from_string(string: &str) -> Self {
        let mut result = IndexMap::new();
        for pair in string.split(",") {
            let mut parts = pair.split(':');
            let key = match parts.next() {
                Some(key) => key.to_string(),
                None => panic!("Missing key"),
            };
            let value = match parts.next() {
                Some(value) => match value.parse::<i64>() {
                    Ok(value) => value,
                    Err(_) => panic!("Failed to parse value"),
                },
                None => panic!("Missing value"),
            };
            result.insert(key, value);
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_tables_id_info_convert_to_string() {
        let mut tables_id_info = IndexMap::new();
        tables_id_info.insert("employees".to_string(), 3);
        tables_id_info.insert("departments".to_string(), 5);
        assert!(
            // Order inside a hashmap is not guaranteed
            (tables_id_info.convert_to_string() == "employees:3,departments:5") || (tables_id_info.convert_to_string() == "departments:5,employees:3")
        );
    }

    #[test]
    fn test_tables_id_info_from_string() {
        let tables_id_info = TablesIdInfo::from_string("employees:3,departments:5");
        let mut expected = IndexMap::new();
        expected.insert("employees".to_string(), 3);
        expected.insert("departments".to_string(), 5);
        assert_eq!(tables_id_info, expected);
    }
}