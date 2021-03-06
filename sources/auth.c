
/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include <machinarium.h>
#include <shapito.h>

#include "sources/macro.h"
#include "sources/version.h"
#include "sources/atomic.h"
#include "sources/util.h"
#include "sources/error.h"
#include "sources/list.h"
#include "sources/pid.h"
#include "sources/id.h"
#include "sources/logger.h"
#include "sources/daemon.h"
#include "sources/config.h"
#include "sources/config_reader.h"
#include "sources/msg.h"
#include "sources/global.h"
#include "sources/server.h"
#include "sources/server_pool.h"
#include "sources/client.h"
#include "sources/client_pool.h"
#include "sources/route_id.h"
#include "sources/route.h"
#include "sources/route_pool.h"
#include "sources/io.h"
#include "sources/instance.h"
#include "sources/router_cancel.h"
#include "sources/router.h"
#include "sources/system.h"
#include "sources/worker.h"
#include "sources/frontend.h"
#include "sources/backend.h"
#include "sources/auth.h"
#include "sources/auth_query.h"

static inline int
od_auth_frontend_cleartext(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;

	/* AuthenticationCleartextPassword */
	shapito_stream_t *stream = client->stream;
	shapito_stream_reset(stream);
	int rc;
	rc = shapito_be_write_authentication_clear_text(stream);
	if (rc == -1)
		return -1;
	rc = od_write(client->io, stream);
	if (rc == -1) {
		od_error(&instance->logger, "auth", client, NULL,
		         "write error: %s",
		         machine_error(client->io));
		return -1;
	}

	/* wait for password response */
	while (1) {
		shapito_stream_reset(stream);
		rc = od_read(client->io, stream, UINT32_MAX);
		if (rc == -1) {
			od_error(&instance->logger, "auth", client, NULL,
			         "read error: %s",
			         machine_error(client->io));
			return -1;
		}
		shapito_fe_msg_t type = *stream->start;
		od_debug(&instance->logger, "auth", client, NULL, "%s",
		         shapito_fe_msg_to_string(type));
		if (type == SHAPITO_FE_PASSWORD_MESSAGE)
			break;
	}

	/* read password message */
	shapito_password_t client_token;
	shapito_password_init(&client_token);
	rc = shapito_be_read_password(&client_token, stream->start,
	                              shapito_stream_used(stream));
	if (rc == -1) {
		od_error(&instance->logger, "auth", client, NULL,
		         "password read error");
		od_frontend_error(client, SHAPITO_PROTOCOL_VIOLATION,
		                  "bad password message");
		shapito_password_free(&client_token);
		return -1;
	}

	/* use remote or local password source */
	shapito_password_t client_password;
	shapito_password_init(&client_password);

	if (client->config->auth_query) {
		rc = od_auth_query(client->global,
		                   stream,
		                   client->config,
		                   client->startup.user,
		                   &client_password);
		if (rc == -1) {
			od_error(&instance->logger, "auth", client, NULL,
			         "failed to make auth_query");
			od_frontend_error(client, SHAPITO_INVALID_AUTHORIZATION_SPECIFICATION,
			                  "failed to make auth query");
			shapito_password_free(&client_token);
			shapito_password_free(&client_password);
			return -1;
		}
	} else {
		client_password.password_len = client->config->password_len + 1;
		client_password.password     = client->config->password;
	}

	/* authenticate */
	int check = shapito_password_compare(&client_password, &client_token);
	shapito_password_free(&client_token);
	if (client->config->auth_query)
		shapito_password_free(&client_password);
	if (! check) {
		od_log(&instance->logger, "auth", client, NULL,
		       "user '%s.%s' incorrect password",
		       shapito_parameter_value(client->startup.database),
		       shapito_parameter_value(client->startup.user));
		od_frontend_error(client, SHAPITO_INVALID_PASSWORD,
		                  "incorrect password");
		return -1;
	}
	return 0;
}

static inline int
od_auth_frontend_md5(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;

	/* generate salt */
	uint32_t salt = shapito_password_salt(&client->key);

	/* AuthenticationMD5Password */
	shapito_stream_t *stream = client->stream;
	shapito_stream_reset(stream);
	int rc;
	rc = shapito_be_write_authentication_md5(stream, (char*)&salt);
	if (rc == -1)
		return -1;
	rc = od_write(client->io, stream);
	if (rc == -1) {
		od_error(&instance->logger, "auth", client, NULL,
		         "write error: %s",
		         machine_error(client->io));
		return -1;
	}

	/* wait for password response */
	while (1) {
		int rc;
		shapito_stream_reset(stream);
		rc = od_read(client->io, stream, UINT32_MAX);
		if (rc == -1) {
			od_error(&instance->logger, "auth", client, NULL,
			         "read error: %s",
			         machine_error(client->io));
			return -1;
		}
		shapito_fe_msg_t type = *stream->start;
		od_debug(&instance->logger, "auth", client, NULL, "%s",
		         shapito_fe_msg_to_string(type));
		if (type == SHAPITO_FE_PASSWORD_MESSAGE)
			break;
	}

	/* read password message */
	shapito_password_t client_token;
	shapito_password_init(&client_token);
	rc = shapito_be_read_password(&client_token, stream->start,
	                              shapito_stream_used(stream));
	if (rc == -1) {
		od_error(&instance->logger, "auth", client, NULL,
		         "password read error");
		od_frontend_error(client, SHAPITO_PROTOCOL_VIOLATION,
		                  "bad password message");
		shapito_password_free(&client_token);
		return -1;
	}

	/* use remote or local password source */
	shapito_password_t client_password;
	shapito_password_init(&client_password);

	shapito_password_t query_password;
	shapito_password_init(&query_password);

	if (client->config->auth_query) {
		rc = od_auth_query(client->global,
		                   stream,
		                   client->config,
		                   client->startup.user,
		                   &query_password);
		if (rc == -1) {
			od_error(&instance->logger, "auth", client, NULL,
			         "failed to make auth_query");
			od_frontend_error(client, SHAPITO_INVALID_AUTHORIZATION_SPECIFICATION,
			                  "failed to make auth query");
			shapito_password_free(&client_token);
			shapito_password_free(&query_password);
			return -1;
		}
		query_password.password_len--;
	} else {
		query_password.password_len = client->config->password_len;
		query_password.password = client->config->password;
	}

	/* prepare password hash */
	rc = shapito_password_md5(&client_password,
	                          shapito_parameter_value(client->startup.user),
	                          client->startup.user->value_len - 1,
	                          query_password.password,
	                          query_password.password_len,
	                          (char*)&salt);
	if (rc == -1) {
		od_error(&instance->logger, "auth", client, NULL,
		         "memory allocation error");
		shapito_password_free(&client_password);
		shapito_password_free(&client_token);
		if (client->config->auth_query)
			shapito_password_free(&query_password);
		return -1;
	}

	/* authenticate */
	int check = shapito_password_compare(&client_password, &client_token);
	shapito_password_free(&client_password);
	shapito_password_free(&client_token);
	if (client->config->auth_query)
		shapito_password_free(&query_password);
	if (! check) {
		od_log(&instance->logger, "auth", client, NULL,
		       "user '%s.%s' incorrect password",
		       shapito_parameter_value(client->startup.database),
		       shapito_parameter_value(client->startup.user));
		od_frontend_error(client, SHAPITO_INVALID_PASSWORD,
		                  "incorrect password");
		return -1;
	}
	return 0;
}

static inline int
od_auth_frontend_cert(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;
	if (! client->startup.is_ssl_request) {
		od_error(&instance->logger, "auth", client, NULL,
		         "TLS connection required");
		od_frontend_error(client, SHAPITO_INVALID_AUTHORIZATION_SPECIFICATION,
		                  "TLS connection required");
		return -1;
	}

	/* compare client certificate common name */
	od_route_t *route = client->route;
	int rc;
	if (route->config->auth_common_name_default) {
		rc = machine_io_verify(client->io, route->config->user_name);
		if (! rc) {
			return 0;
		}
	}

	od_list_t *i;
	od_list_foreach(&route->config->auth_common_names, i) {
		od_configauth_t *auth;
		auth = od_container_of(i, od_configauth_t, link);
		rc = machine_io_verify(client->io, auth->common_name);
		if (! rc) {
			return 0;
		}
	}

	od_error(&instance->logger, "auth", client, NULL,
	         "TLS certificate common name mismatch");
	od_frontend_error(client, SHAPITO_INVALID_PASSWORD,
	                  "TLS certificate common name mismatch");
	return -1;
}

static inline int
od_auth_frontend_block(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;
	od_log(&instance->logger, "auth", client, NULL,
	       "user '%s.%s' is blocked",
	       shapito_parameter_value(client->startup.database),
	       shapito_parameter_value(client->startup.user));
	od_frontend_error(client, SHAPITO_INVALID_AUTHORIZATION_SPECIFICATION,
	                  "user blocked");
	return 0;
}

int od_auth_frontend(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;

	/* authentication mode */
	int rc;
	switch (client->config->auth_mode) {
	case OD_AUTH_CLEAR_TEXT:
		rc = od_auth_frontend_cleartext(client);
		if (rc == -1)
			return -1;
		break;
	case OD_AUTH_MD5:
		rc = od_auth_frontend_md5(client);
		if (rc == -1)
			return -1;
		break;
	case OD_AUTH_CERT:
		rc = od_auth_frontend_cert(client);
		if (rc == -1)
			return -1;
		break;
	case OD_AUTH_BLOCK:
		od_auth_frontend_block(client);
		return -1;
	case OD_AUTH_NONE:
		break;
	default:
		assert(0);
		break;
	}

	/* pass */
	shapito_stream_t *stream = client->stream;
	shapito_stream_reset(stream);
	rc = shapito_be_write_authentication_ok(stream);
	if (rc == -1)
		return -1;
	rc = od_write(client->io, stream);
	if (rc == -1) {
		od_error(&instance->logger, "auth", client, NULL,
		         "write error: %s",
		         machine_error(client->io));
		return -1;
	}
	return 0;
}

static inline int
od_auth_backend_cleartext(od_server_t *server, shapito_stream_t *stream)
{
	od_instance_t *instance = server->global->instance;
	od_route_t *route = server->route;
	assert(route != NULL);

	od_debug(&instance->logger, "auth", NULL, server,
	         "requested clear-text authentication");

	/* use storage or user password */
	char *password;
	int   password_len;
	if (route->config->storage_password) {
		password = route->config->storage_password;
		password_len = route->config->storage_password_len;
	} else
	if (route->config->password) {
		password = route->config->password;
		password_len = route->config->password_len;
	} else {
		od_error(&instance->logger, "auth", NULL, server,
		         "password required for route '%s.%s'",
		         route->config->db_name,
		         route->config->user_name);
		return -1;
	}

	/* PasswordMessage */
	shapito_stream_reset(stream);
	int rc;
	rc = shapito_fe_write_password(stream, password, password_len + 1);
	if (rc == -1) {
		od_error(&instance->logger, "auth", NULL, server,
		         "memory allocation error");
		return -1;
	}
	rc = od_write(server->io, stream);
	if (rc == -1) {
		od_error(&instance->logger, "auth", NULL, server,
		         "write error: %s",
		         machine_error(server->io));
		return -1;
	}
	return 0;
}

static inline int
od_auth_backend_md5(od_server_t *server, shapito_stream_t *stream,
                    char salt[4])
{
	od_instance_t *instance = server->global->instance;
	od_route_t *route = server->route;
	assert(route != NULL);

	od_debug(&instance->logger, "auth", NULL, server,
	         "requested md5 authentication");

	/* use storage user or route user */
	char *user;
	int   user_len;
	if (route->config->storage_user) {
		user = route->config->storage_user;
		user_len = route->config->storage_user_len;
	} else {
		user = route->config->user_name;
		user_len = route->config->user_name_len;
	}

	/* use storage or user password */
	char *password;
	int   password_len;
	if (route->config->storage_password) {
		password = route->config->storage_password;
		password_len = route->config->storage_password_len;
	} else
	if (route->config->password) {
		password = route->config->password;
		password_len = route->config->password_len;
	} else {
		od_error(&instance->logger, "auth", NULL, server,
		         "password required for route '%s.%s'",
		         route->config->db_name,
		         route->config->user_name);
		return -1;
	}

	/* prepare md5 password using server supplied salt */
	shapito_password_t client_password;
	shapito_password_init(&client_password);
	int rc;
	rc = shapito_password_md5(&client_password, user, user_len,
	                          password,
	                          password_len, salt);
	if (rc == -1) {
		od_error(&instance->logger, "auth", NULL, server,
		         "memory allocation error");
		shapito_password_free(&client_password);
		return -1;
	}

	/* PasswordMessage */
	shapito_stream_reset(stream);
	rc = shapito_fe_write_password(stream,
	                               client_password.password,
	                               client_password.password_len);
	shapito_password_free(&client_password);
	if (rc == -1) {
		od_error(&instance->logger, "auth", NULL, server,
		         "memory allocation error");
		return -1;
	}
	rc = od_write(server->io, stream);
	if (rc == -1) {
		od_error(&instance->logger, "auth", NULL, server,
		         "write error: %s",
		         machine_error(server->io));
		return -1;
	}
	return 0;
}

int od_auth_backend(od_server_t *server, shapito_stream_t *stream)
{
	od_instance_t *instance = server->global->instance;

	assert(*stream->start == SHAPITO_BE_AUTHENTICATION);

	uint32_t auth_type;
	char salt[4];
	int rc;
	rc = shapito_fe_read_auth(&auth_type, salt, stream->start,
	                          shapito_stream_used(stream));
	if (rc == -1) {
		od_error(&instance->logger, "auth", NULL, server,
		         "failed to parse authentication message");
		return -1;
	}
	switch (auth_type) {
	/* AuthenticationOk */
	case 0:
		return 0;
	/* AuthenticationCleartextPassword */
	case 3:
		rc = od_auth_backend_cleartext(server, stream);
		if (rc == -1)
			return -1;
		break;
	/* AuthenticationMD5Password */
	case 5:
		rc = od_auth_backend_md5(server, stream, salt);
		if (rc == -1)
			return -1;
		break;
	/* unsupported */
	default:
		od_error(&instance->logger, "auth", NULL, server,
		         "unsupported authentication method");
		return -1;
	}

	/* wait for authentication response */
	while (1) {
		int rc;
		shapito_stream_reset(stream);
		rc = od_read(server->io, stream, UINT32_MAX);
		if (rc == -1) {
			od_error(&instance->logger, "auth", NULL, server,
			         "read error: %s",
			         machine_error(server->io));
			return -1;
		}
		shapito_be_msg_t type = *stream->start;
		od_debug(&instance->logger, "auth", NULL, server, "%s",
		         shapito_be_msg_to_string(type));
		switch (type) {
		case SHAPITO_BE_AUTHENTICATION:
			rc = shapito_fe_read_auth(&auth_type, salt, stream->start,
			                          shapito_stream_used(stream));
			if (rc == -1) {
				od_error(&instance->logger, "auth", NULL, server,
				         "failed to parse authentication message");
				return -1;
			}
			if (auth_type != 0) {
				od_error(&instance->logger, "auth", NULL, server,
				         "incorrect authentication flow");
				return 0;
			}
			return 0;
		case SHAPITO_BE_ERROR_RESPONSE:
			od_backend_error(server, "auth", stream->start,
			                 shapito_stream_used(stream));
			return -1;
		default:
			break;
		}
	}
	return 0;
}
