/*
 * sql_postgresql.c		Postgresql rlm_sql driver
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2000  Mike Machado <mike@innercite.com>
 * Copyright 2000  Alan DeKok <aland@ox.org>
 */

/*
 * April 2001:
 *
 * Use blocking queries and delete unused functions. In
 * rlm_sql_postgresql replace all functions that are not really used
 * with the not_implemented function.
 *
 * Add a new field to the rlm_sql_postgres_conn_t struct to store the
 * number of rows affected by a query because the sql module calls
 * finish_query before it retrieves the number of affected rows from the
 * driver
 *
 * Bernhard Herzog <bh@intevation.de>
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>

#include <sys/stat.h>

#include <libpq-fe.h>
#include <postgres_ext.h>

#include "config.h"
#include "rlm_sql.h"
#include "sql_postgresql.h"

#ifndef NAMEDATALEN
#  define NAMEDATALEN 64
#endif

typedef struct rlm_sql_postgres_config {
	char const	*db_string;
	bool		send_application_name;
} rlm_sql_postgres_config_t;

typedef struct rlm_sql_postgres_conn {
	PGconn		*db;
	PGresult	*result;
	int		cur_row;
	int		num_fields;
	int		affected_rows;
	char		**row;
} rlm_sql_postgres_conn_t;

static CONF_PARSER driver_config[] = {
	{ "send_application_name", FR_CONF_OFFSET(PW_TYPE_BOOLEAN, rlm_sql_postgres_config_t, send_application_name), "yes" },

	{ NULL, -1, 0, NULL, NULL }
};

static int mod_instantiate(CONF_SECTION *conf, UNUSED rlm_sql_config_t *config)
{
#if defined(HAVE_OPENSSL_CRYPTO_H) && (defined(HAVE_PQINITOPENSSL) || defined(HAVE_PQINITSSL))
	static bool			ssl_init = false;
#endif

	rlm_sql_postgres_config_t	*driver;
	char 				application_name[NAMEDATALEN];
	char				*db_string;

#if defined(HAVE_OPENSSL_CRYPTO_H) && (defined(HAVE_PQINITOPENSSL) || defined(HAVE_PQINITSSL))
	if (!ssl_init) {
#  ifdef HAVE_PQINITOPENSSL
		PQinitOpenSSL(0, 0);
#  else
		PQinitSSL(0);
#  endif
		ssl_init = true;
	}
#endif

	MEM(driver = config->driver = talloc_zero(config, rlm_sql_postgres_config_t));
	if (cf_section_parse(conf, driver, driver_config) < 0) {
		return -1;
	}

	db_string = strchr(config->sql_db, '=') ?
		talloc_typed_strdup(driver, config->sql_db) :
		talloc_typed_asprintf(driver, "dbname='%s'", config->sql_db);

	if (config->sql_server[0] != '\0') {
		db_string = talloc_asprintf_append(db_string, " host='%s'", config->sql_server);
	}

	if (config->sql_port[0] != '\0') {
		db_string = talloc_asprintf_append(db_string, " port=%s", config->sql_port);
	}

	if (config->sql_login[0] != '\0') {
		db_string = talloc_asprintf_append(db_string, " user='%s'", config->sql_login);
	}

	if (config->sql_password[0] != '\0') {
		db_string = talloc_asprintf_append(db_string, " password='%s'", config->sql_password);
	}

	/*
	 *	Allow the user to set their own, or disable it
	 */
	if (driver->send_application_name) {
		snprintf(application_name, sizeof(application_name),
			 "FreeRADIUS "  RADIUSD_VERSION_STRING " - %s (%s)", progname, config->xlat_name);
		db_string = talloc_asprintf_append(db_string, " application_name='%s'", application_name);
	}
	driver->db_string = db_string;

	return 0;
}

/** Return the number of affected rows of the result as an int instead of the string that postgresql provides
 *
 */
static int affected_rows(PGresult * result)
{
	return atoi(PQcmdTuples(result));
}

/** Free the row of the current result that's stored in the conn struct
 *
 */
static void free_result_row(rlm_sql_postgres_conn_t *conn)
{
	TALLOC_FREE(conn->row);
	conn->num_fields = 0;
}

#if defined(PG_DIAG_SQLSTATE) && defined(PG_DIAG_MESSAGE_PRIMARY)
static sql_rcode_t sql_classify_error(PGresult const *result)
{
	int i;

	char *errorcode;
	char *errormsg;

	/*
	 *	Check the error code to see if we should reconnect or not
	 *	Error Code table taken from:
	 *	http://www.postgresql.org/docs/8.1/interactive/errcodes-appendix.html
	 */
	errorcode = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	errormsg = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
	if (!errorcode) {
		ERROR("rlm_sql_postgresql: Error occurred, but unable to retrieve error code");
		return RLM_SQL_ERROR;
	}

	/* SUCCESSFUL COMPLETION */
	if (strcmp("00000", errorcode) == 0) {
		return RLM_SQL_OK;
	}

	/* WARNING */
	if (strcmp("01000", errorcode) == 0) {
		WARN("%s", errormsg);
		return RLM_SQL_OK;
	}

	/* UNIQUE VIOLATION */
	if (strcmp("23505", errorcode) == 0) {
		return RLM_SQL_DUPLICATE;
	}

	/* others */
	for (i = 0; errorcodes[i].errorcode != NULL; i++) {
		if (strcmp(errorcodes[i].errorcode, errorcode) == 0) {
			ERROR("rlm_sql_postgresql: %s: %s", errorcode, errorcodes[i].meaning);

			return (errorcodes[i].reconnect == true) ?
				RLM_SQL_RECONNECT :
				RLM_SQL_ERROR;
		}
	}

	ERROR("rlm_sql_postgresql: Can't classify: %s", errorcode);
	return RLM_SQL_ERROR;
}
#  else
static sql_rcode_t sql_classify_error(UNUSED PGresult const *result)
{
	ERROR("rlm_sql_postgresql: Error occurred, no more information available, rebuild with newer libpq");
	return RLM_SQL_ERROR;
}
#endif

static int _sql_socket_destructor(rlm_sql_postgres_conn_t *conn)
{
	DEBUG2("rlm_sql_postgresql: Socket destructor called, closing socket");

	if (!conn->db) {
		return 0;
	}

	/* PQfinish also frees the memory used by the PGconn structure */
	PQfinish(conn->db);
	conn->db = NULL;

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_create_socket
 *
 *	Purpose: Establish connection to the db
 *
 *************************************************************************/
static int CC_HINT(nonnull) sql_init_socket(rlm_sql_handle_t *handle, rlm_sql_config_t *config)
{
	rlm_sql_postgres_config_t *driver = config->driver;
	rlm_sql_postgres_conn_t *conn;

	MEM(conn = handle->conn = talloc_zero(handle, rlm_sql_postgres_conn_t));
	talloc_set_destructor(conn, _sql_socket_destructor);

	DEBUG2("rlm_sql_postgresql: Connecting using parameters: %s", driver->db_string);
	conn->db = PQconnectdb(driver->db_string);
	if (!conn->db) {
		ERROR("rlm_sql_postgresql: Connection failed: Out of memory");
		return -1;
	}
	if (PQstatus(conn->db) != CONNECTION_OK) {
		ERROR("rlm_sql_postgresql: Connection failed: %s", PQerrorMessage(conn->db));
		PQfinish(conn->db);
		conn->db = NULL;
		return -1;
	}

	DEBUG2("Connected to database '%s' on '%s' server version %i, protocol version %i, backend PID %i ",
	       PQdb(conn->db), PQhost(conn->db), PQserverVersion(conn->db), PQprotocolVersion(conn->db),
	       PQbackendPID(conn->db));

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_query
 *
 *	Purpose: Issue a query to the database
 *
 *************************************************************************/
static CC_HINT(nonnull) sql_rcode_t sql_query(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config,
					      char const *query)
{
	rlm_sql_postgres_conn_t *conn = handle->conn;
	ExecStatusType status;
	int numfields = 0;

	if (!conn->db) {
		ERROR("rlm_sql_postgresql: Socket not connected");
		return RLM_SQL_RECONNECT;
	}

	/*
	 *  Returns a PGresult pointer or possibly a null pointer.
	 *  A non-null pointer will generally be returned except in
	 *  out-of-memory conditions or serious errors such as inability
	 *  to send the command to the server. If a null pointer is
	 *  returned, it should be treated like a PGRES_FATAL_ERROR
	 *  result.
	 */
	conn->result = PQexec(conn->db, query);

	/*
	 *  As this error COULD be a connection error OR an out-of-memory
	 *  condition return value WILL be wrong SOME of the time
	 *  regardless! Pick your poison...
	 */
	if (!conn->result) {
		ERROR("rlm_sql_postgresql: Failed getting query result: %s", PQerrorMessage(conn->db));
		return RLM_SQL_RECONNECT;
	}

	status = PQresultStatus(conn->result);
	DEBUG("rlm_sql_postgresql: Status: %s", PQresStatus(status));

	switch (status){
	/*
	 *  Successful completion of a command returning no data.
	 */
	case PGRES_COMMAND_OK:
		/*
		 *  Affected_rows function only returns the number of affected rows of a command
		 *  returning no data...
		 */
		conn->affected_rows = affected_rows(conn->result);
		DEBUG("rlm_sql_postgresql: query affected rows = %i", conn->affected_rows);
		return RLM_SQL_OK;
	/*
	 *  Successful completion of a command returning data (such as a SELECT or SHOW).
	 */
#ifdef HAVE_PGRES_SINGLE_TUPLE
	case PGRES_SINGLE_TUPLE:
#endif
	case PGRES_TUPLES_OK:
		conn->cur_row = 0;
		conn->affected_rows = PQntuples(conn->result);
		numfields = PQnfields(conn->result); /*Check row storing functions..*/
		DEBUG("rlm_sql_postgresql: query affected rows = %i , fields = %i", conn->affected_rows, numfields);
		return RLM_SQL_OK;

#ifdef HAVE_PGRES_COPY_BOTH
	case PGRES_COPY_BOTH:
#endif
	case PGRES_COPY_OUT:
	case PGRES_COPY_IN:
		DEBUG("rlm_sql_postgresql: Data transfer started");
		return RLM_SQL_OK;

	/*
	 *  Weird.. this shouldn't happen.
	 */
	case PGRES_EMPTY_QUERY:
		ERROR("rlm_sql_postgresql: Empty query");
		return RLM_SQL_QUERY_ERROR;

	/*
	 *  The server's response was not understood.
	 */
	case PGRES_BAD_RESPONSE:
		ERROR("rlm_sql_postgresql: Bad Response From Server");
		return RLM_SQL_RECONNECT;


	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
		return sql_classify_error(conn->result);
	}

	return RLM_SQL_ERROR;
}


/*************************************************************************
 *
 *	Function: sql_select_query
 *
 *	Purpose: Issue a select query to the database
 *
 *************************************************************************/
static sql_rcode_t sql_select_query(rlm_sql_handle_t * handle, rlm_sql_config_t *config, char const *query) {
	return sql_query(handle, config, query);
}

/*************************************************************************
 *
 *	Function: sql_fetch_row
 *
 *	Purpose: database specific fetch_row. Returns a rlm_sql_row_t struct
 *	with all the data for the query in 'handle->row'. Returns
 *	0 on success, -1 on failure, RLM_SQL_RECONNECT if 'database is down'.
 *
 *************************************************************************/
static sql_rcode_t sql_fetch_row(rlm_sql_handle_t * handle, UNUSED rlm_sql_config_t *config) {

	int records, i, len;
	rlm_sql_postgres_conn_t *conn = handle->conn;

	handle->row = NULL;

	if (conn->cur_row >= PQntuples(conn->result))
		return 0;

	free_result_row(conn);

	records = PQnfields(conn->result);
	conn->num_fields = records;

	if ((PQntuples(conn->result) > 0) && (records > 0)) {
		conn->row = talloc_zero_array(conn, char *, records + 1);
		for (i = 0; i < records; i++) {
			len = PQgetlength(conn->result, conn->cur_row, i);
			conn->row[i] = talloc_array(conn->row, char, len + 1);
			strlcpy(conn->row[i], PQgetvalue(conn->result, conn->cur_row, i),len + 1);
		}
		conn->cur_row++;
		handle->row = conn->row;
	}

	return 0;
}

/*************************************************************************
 *
 *      Function: sql_num_fields
 *
 *      Purpose: database specific num_fields. Returns number of rows in
 *	       query
 *
 *************************************************************************/
static int sql_num_fields(rlm_sql_handle_t * handle, UNUSED rlm_sql_config_t *config)
{
	rlm_sql_postgres_conn_t *conn = handle->conn;

	conn->affected_rows = PQntuples(conn->result);
	if (conn->result)
		return PQnfields(conn->result);

	return 0;
}



/*************************************************************************
 *
 *	Function: sql_free_result
 *
 *	Purpose: database specific free_result. Frees memory allocated
 *	       for a result set
 *
 *************************************************************************/
static sql_rcode_t sql_free_result(rlm_sql_handle_t * handle, UNUSED rlm_sql_config_t *config) {

	rlm_sql_postgres_conn_t *conn = handle->conn;

	if (conn->result != NULL) {
		PQclear(conn->result);
		conn->result = NULL;
	}

	free_result_row(conn);

	return 0;
}



/*************************************************************************
 *
 *	Function: sql_error
 *
 *	Purpose: database specific error. Returns error associated with
 *	       connection
 *
 *************************************************************************/
static char const *sql_error(rlm_sql_handle_t * handle, UNUSED rlm_sql_config_t *config) {

	rlm_sql_postgres_conn_t *conn = handle->conn;

	return PQerrorMessage(conn->db);
}

/*************************************************************************
 *
 *	Function: sql_affected_rows
 *
 *	Purpose: Return the number of rows affected by the last query.
 *
 *************************************************************************/
static int sql_affected_rows(rlm_sql_handle_t * handle, UNUSED rlm_sql_config_t *config) {
	rlm_sql_postgres_conn_t *conn = handle->conn;

	return conn->affected_rows;
}

/* Exported to rlm_sql */
rlm_sql_module_t rlm_sql_postgresql = {
	"rlm_sql_postgresql",
	mod_instantiate,
	sql_init_socket,
	sql_query,
	sql_select_query,
	NULL, /* sql_store_result */
	sql_num_fields,
	NULL, /* sql_num_rows */
	sql_fetch_row,
	NULL, /* sql_free_result */
	sql_error,
	sql_free_result,
	sql_free_result,
	sql_affected_rows,
};
