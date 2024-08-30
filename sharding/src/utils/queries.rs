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
    query_is(query, QueryTypes::INSERT) ||
    query_is(query, QueryTypes::DELETE) ||
    query_is(query, QueryTypes::DROP) ||
    query_is(query, QueryTypes::UPDATE) ||
    query_is(query, QueryTypes::CREATE)
}