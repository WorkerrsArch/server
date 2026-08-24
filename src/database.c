// database.c — работа с PostgreSQL на Railway
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>

static PGconn *dbConn = NULL;

bool Database_Connect(const char *connStr) {
    if (dbConn) PQfinish(dbConn);
    
    dbConn = PQconnectdb(connStr);
    if (PQstatus(dbConn) != CONNECTION_OK) {
        fprintf(stderr, "Database connection failed: %s\n", PQerrorMessage(dbConn));
        PQfinish(dbConn);
        dbConn = NULL;
        return false;
    }
    printf("Connected to PostgreSQL\n");
    return true;
}

bool Database_ConnectFromEnv(void) {
    // Если DATABASE_URL не установлена, то локальное развитие
    const char *dbUrl = getenv("DATABASE_URL");
    if (!dbUrl) {
        printf("DATABASE_URL not set, skipping DB init (local dev mode)\n");
        return false;
    }
    return Database_Connect(dbUrl);
}

PGconn *Database_GetConnection(void) {
    return dbConn;
}

void Database_Disconnect(void) {
    if (dbConn) {
        PQfinish(dbConn);
        dbConn = NULL;
    }
}

bool Database_ExecuteQuery(const char *query) {
    if (!dbConn) return false;
    PGresult *res = PQexec(dbConn, query);
    bool success = (PQresultStatus(res) == PGRES_COMMAND_OK || PQresultStatus(res) == PGRES_TUPLES_OK);
    if (!success) {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage(dbConn));
    }
    PQclear(res);
    return success;
}

PGresult *Database_ExecuteQueryResult(const char *query) {
    if (!dbConn) return NULL;
    return PQexec(dbConn, query);
}

void Database_FreeResult(PGresult *res) {
    if (res) PQclear(res);
}
