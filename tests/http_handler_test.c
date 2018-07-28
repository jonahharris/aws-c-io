
#include <aws/testing/aws_test_harness.h>

#include <aws/io/channel.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/socket.h>

#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>

struct aws_http_connection_options {
    struct aws_socket_options socket_options;
    struct aws_socket_endpoint socket_endpoint;
};

struct aws_http_connection {
    struct aws_event_loop *loop;
    void *bootstrap;
    struct aws_channel *channel;
    struct aws_channel_handler *handler;
    struct aws_channel_slot *slot;
};

static void test_http_common_options(struct aws_http_connection_options *options, const char *socket_name, const char *port) {
    struct aws_socket_options socket_options = {
        .type = AWS_SOCKET_STREAM,
        .domain = AWS_SOCKET_IPV6,
        .linger_time = 0,
        .read_timeout = 0,
        .send_timeout = 0,
        .connect_timeout = 3000,
        .keepalive = false
    };
    options->socket_options = socket_options;

    struct aws_socket_endpoint socket_endpoint;
    AWS_ZERO_STRUCT(socket_endpoint);
    sprintf(socket_endpoint.address, "::1");
    sprintf(socket_endpoint.socket_name, "%s", socket_name);
    sprintf(socket_endpoint.port, "%s", port);
    options->socket_endpoint = socket_endpoint;
}

static void test_http_client_options(struct aws_http_connection_options *options) {
    test_http_common_options(options, "test_http_client", "1500");
}

static void test_http_server_options(struct aws_http_connection_options *options) {
    test_http_common_options(options, "test_http_server", "1500");
}

static int aws_http_client_setup_callback(struct aws_client_bootstrap *bootstrap, int error_code, struct aws_channel *channel, void *user_data) {
    (void)bootstrap;

    if (error_code) {
        return AWS_OP_ERR;
    }

    struct aws_http_connection *connection = (struct aws_http_connection *)user_data;
    connection->channel = channel;
    struct aws_channel_slot *slot = aws_channel_slot_new(channel);
    aws_channel_slot_insert_end(channel, slot);
    aws_channel_slot_set_handler(slot, connection->handler);
    connection->slot = slot;
    connection->loop = channel->loop;

    return AWS_OP_SUCCESS;
}

static int aws_http_client_shutdown_callback(struct aws_client_bootstrap *bootstrap, int error_code, struct aws_channel *channel, void *user_data) {
    return AWS_OP_SUCCESS;
}

static int aws_http_server_setup_callback(struct aws_server_bootstrap *bootstrap, int error_code, struct aws_channel *channel, void *user_data) {
    (void)bootstrap;

    if (error_code) {
        return AWS_OP_ERR;
    }

    struct aws_http_connection *connection = (struct aws_http_connection *)user_data;
    connection->channel = channel;
    struct aws_channel_slot *slot = aws_channel_slot_new(channel);
    aws_channel_slot_insert_end(channel, slot);
    aws_channel_slot_set_handler(slot, connection->handler);
    connection->slot = slot;

    return AWS_OP_SUCCESS;
}

static int aws_http_server_shutdown_callback(struct aws_server_bootstrap *bootstrap, int error_code, struct aws_channel *channel, void *user_data) {
    return AWS_OP_SUCCESS;
}

static int aws_http_handler_process_read_message(struct aws_channel_handler *handler, struct aws_channel_slot *slot,
                                                 struct aws_io_message *message) {
    return AWS_OP_SUCCESS;
}

static int aws_http_handler_process_write_message(struct aws_channel_handler *handler, struct aws_channel_slot *slot,
                                                  struct aws_io_message *message) {
    return AWS_OP_SUCCESS;
}

static int aws_http_handler_increment_read_window(struct aws_channel_handler *handler, struct aws_channel_slot *slot, size_t size) {
    return AWS_OP_SUCCESS;
}

static int aws_http_handler_shutdown(struct aws_channel_handler *handler, struct aws_channel_slot *slot,
                                     enum aws_channel_direction dir, int error_code, bool abort_immediately) {
    return AWS_OP_SUCCESS;
}

static size_t aws_http_handler_get_current_window_size(struct aws_channel_handler *handler) {
    return 0;
}

static void aws_http_handler_destroy(struct aws_channel_handler *handler) {
    struct aws_http_connection *connection = (struct aws_http_connection *)handler->impl;
    if (aws_event_loop_thread_is_callers_thread(connection->loop)) {
    }
}

static struct aws_channel_handler *aws_http_channel_handler_new(struct aws_allocator *alloc, struct aws_http_connection *connection) {
    struct aws_channel_handler *handler = (struct aws_channel_handler *)aws_mem_acquire(alloc, sizeof(struct aws_channel_handler));
    handler->alloc = alloc;
    handler->vtable = (struct aws_channel_handler_vtable){
        .process_read_message = aws_http_handler_process_read_message,
        .process_write_message = aws_http_handler_process_write_message,
        .increment_read_window = aws_http_handler_increment_read_window,
        .shutdown = aws_http_handler_shutdown,
        .initial_window_size = aws_http_handler_get_current_window_size,
        .destroy = aws_http_handler_destroy
    };
    handler->impl = (void *)connection;
    return handler;
}

int aws_http_client_init(struct aws_allocator *alloc, struct aws_http_connection_options *options,
                         struct aws_http_connection *connection, struct aws_event_loop_group *event_loop_group) {
    struct aws_channel_handler *handler = aws_http_channel_handler_new(alloc, connection);
    connection->handler = handler;
    connection->slot = NULL;
    connection->loop = NULL;

    struct aws_client_bootstrap client_bootstrap;
    ASSERT_SUCCESS(aws_client_bootstrap_init(&client_bootstrap, alloc, event_loop_group));

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&client_bootstrap, &options->socket_endpoint,
                                                           &options->socket_options, aws_http_client_setup_callback,
                                                           aws_http_client_shutdown_callback, (void*)connection));

    return AWS_OP_SUCCESS;
}

struct aws_socket *aws_http_server_init(struct aws_allocator *alloc, struct aws_http_connection_options *options,
                                        struct aws_http_connection *connection, struct aws_event_loop_group *event_loop_group) {
    struct aws_channel_handler *handler = aws_http_channel_handler_new(alloc, connection);
    connection->handler = handler;

    struct aws_server_bootstrap server_bootstrap;
    int ret = aws_server_bootstrap_init(&server_bootstrap, alloc, event_loop_group);
    if (ret != AWS_OP_SUCCESS) {
        return NULL;
    }

    struct aws_socket *listener = aws_server_bootstrap_add_socket_listener(&server_bootstrap,
                                                                           &options->socket_endpoint, &options->socket_options, aws_http_server_setup_callback, aws_http_server_shutdown_callback, NULL);

    return listener;
}

static void test_http_client_clean_up(struct aws_http_connection * connection) {
    aws_client_bootstrap_clean_up((struct aws_client_bootstrap *)connection->bootstrap);
}

static void test_http_server_clean_up(struct aws_http_connection *connection, struct aws_socket *listener) {
    aws_server_bootstrap_remove_socket_listener((struct aws_server_bootstrap *)connection->bootstrap, listener);
    aws_server_bootstrap_clean_up((struct aws_server_bootstrap *)connection->bootstrap);
}

AWS_TEST_CASE(http_stuff, http_stuff_fn)
static int http_stuff_fn(struct aws_allocator *allocator, void *user_data) {
    (void)allocator;
    (void)user_data;

    /* Setup event loop to handle IO. */
    struct aws_event_loop_group event_loop_group;
    ASSERT_SUCCESS(aws_event_loop_group_default_init(&event_loop_group, allocator));

    /* Setup client. */
    struct aws_http_connection_options client_options;
    test_http_client_options(&client_options);

    struct aws_http_connection client_connection;
    ASSERT_SUCCESS(aws_http_client_init(allocator, &client_options, &client_connection, &event_loop_group));

    /* Setup server. */
    struct aws_http_connection_options server_options;
    test_http_server_options(&server_options);

    struct aws_http_connection server_connection;
    struct aws_socket *listener = aws_http_server_init(allocator, &server_options, &server_connection, &event_loop_group);
    ASSERT_NOT_NULL(listener);

    /* Shutdown. */
    test_http_client_clean_up(&client_connection);
    test_http_server_clean_up(&server_connection, listener);

    aws_event_loop_group_clean_up(&event_loop_group);

    return 0;
}
