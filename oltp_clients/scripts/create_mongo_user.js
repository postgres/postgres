const conn = new Mongo();
const db = conn.getDB("flexi"); // Replace "x" with your database name

// Create the user
db.dropUser("tester")
// use admin
db.createUser({
    user: "tester",
    pwd: "123",
    roles: [{ role: "dbAdminAnyDatabase", db: "admin" }]
});