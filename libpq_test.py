import psycopg2
from psycopg2 import sql

def execute_queries():
    try:
        # Conexión a la base de datos
        connection = psycopg2.connect(
            dbname="template1",
            user="ncontinanza",
            password="",
            host="localhost",
            port="5434"
        )
        cursor = connection.cursor()
        
        # # Query para crear la tabla
        create_table_query = '''
        CREATE TABLE employees (
            id SERIAL PRIMARY KEY,
            name VARCHAR(100),
            position VARCHAR(50),
            salary NUMERIC
        );
        '''
        
        # Query para insertar un registro
        insert_query = '''
        INSERT INTO employees (name, position, salary) 
        VALUES ('Alice Johnson', 'Software Engineer', 85000);
        '''
        
        # Query para seleccionar todos los registros
        select_query = '''
        SELECT * FROM employees;
        '''
        
        # Ejecutar las queries
        cursor.execute(create_table_query)
        cursor.execute(insert_query)
        cursor.execute(select_query)
        
        # Obtener resultados del SELECT
        rows = cursor.fetchall()
        
        # Imprimir resultados
        for row in rows:
            print(row)
        
        # Confirmar cambios en la base de datos
        connection.commit()

    except (Exception, psycopg2.Error) as error:
        print("Error al interactuar con PostgreSQL", error)
    
    finally:
        # Cerrar cursor y conexión
        if connection:
            cursor.close()
            connection.close()
            print("Conexión con PostgreSQL cerrada")

if __name__ == "__main__":
    execute_queries()
