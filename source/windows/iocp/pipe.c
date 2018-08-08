/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/io/pipe.h>

#include <aws/common/task_scheduler.h>
#include <aws/io/event_loop.h>
#include <aws/io/io.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

enum read_end_state {
    /* Pipe is in the process of closing.
     * If the user was subscribed, they no longer receive events and cannot
     * resubscribe without first re-opening the pipe. */
    READ_END_STATE_CLOSING,

    /* Pipe has been opened */
    READ_END_STATE_OPEN,

    /* Pipe is open, user has subscribed, but async monitoring hasn't started yet.
     * Pipe moves to SUBCSCRIBED state if async monitoring starts successfully
     * or SUBSCRIBE_ERROR state if it doesn't start successfully.
     * From any of the SUBSCRIBE* states, the pipe moves to OPEN state if the user unsubscribes. */
    READ_END_STATE_SUBSCRIBING,

    /* Pipe is open, user has subscribed, and user is receiving events delivered by async monitoring.
     * We pause async monitoring once we know the file is readable.
     * We resume async monitoring once the user reads all available bytes.
     * Pipe moves to SUBSCRIBE_ERROR state if async monitoring reports an error, or fails to restart.
     * Pipe move sto OPEN state if user unsubscribes. */
    READ_END_STATE_SUBSCRIBED,

    /* Pipe is open, has subscribed, and we've delivered an error event to the user.
     * We don't bother with delivering further error events and we
     * don't attempt to do async monitoring anymore. */
    READ_END_STATE_SUBSCRIBE_ERROR,
};

/* Reasons to launch async monitoring of the read-end's handle */
enum monitoring_reason {
    MONITORING_BECAUSE_SUBSCRIBING = 1,
    MONITORING_BECAUSE_WAITING_FOR_DATA = 2,
    MONITORING_BECAUSE_ERROR_SUSPECTED = 4,
};

struct read_end_impl {
    struct aws_allocator *alloc;

    enum read_end_state state;

    /* For async zero-byte-read operation that allows us to monitor when the pipe becomes readable */
    struct aws_overlapped overlapped;

    /* True while monitoring-operation, or error-reporting task, is outstanding.
     * Note that rapidly opening/closing or subscribing/unsubscribing could
     * lead to async operations from a previous subscribe still pending
     * while the user is re-subscribing. */
    bool is_async_operation_pending;

    struct aws_io_handle handle; /* Set when opening. */

    struct aws_event_loop *event_loop; /* Set when opening. */

    aws_pipe_on_read_end_closed_fn *on_closed_user_callback; /* Set when entering CLOSING state. */
    void *on_closed_user_data;                               /* Set when entering CLOSING state. */

    aws_pipe_on_read_event_fn
        *on_read_event_user_callback; /* Set when entering SUBSCRIBING state. Reset when exiting one of the SUBSCRIBE*
                                         states for a non-SUBSCRIBE* state. */
    void *on_read_event_user_data;

    /* Reasons to restart monitoring once current async operation completes.
     * Contains read_end_monitoring_request_t flags.*/
    uint8_t
        monitoring_request_reasons; /* Used while in the SUBSCRIBE* states. Reset whenever a new async monitoring task
                                       starts, and also when exiting one of the SUBSCRIBE* for a non-SUBSCRIBE state. */

    /* Events that the error-reporting task will report.
     * Contains aws_io_event_t flags.*/
    uint8_t error_events_to_report; /* Set when entering the SUBSCRIBE_ERROR state. Reset when exiting SUBSCRIBE_ERROR
                                       state. */
};

enum write_end_state {
    WRITE_END_STATE_CLOSING,
    WRITE_END_STATE_OPEN,
};

/* Data describing an async write request */
struct write_request {
    aws_pipe_on_write_complete_fn *user_callback;
    void *user_data;
    struct aws_overlapped overlapped;
    struct aws_linked_list_node list_node;
};

struct write_end_impl {
    struct aws_allocator *alloc;
    enum write_end_state state;
    struct aws_io_handle handle;
    struct aws_event_loop *event_loop;
    struct aws_linked_list write_list; /* List of write_request */
    aws_pipe_on_write_end_closed_fn *on_closed_user_fn;
    void *on_closed_user_data;
};

enum {
    PIPE_BUFFER_SIZE = 4096,
    PIPE_UNIQUE_NAME_MAX_TRIES = 10,
};

static void s_read_end_on_zero_byte_read_completion(
    struct aws_event_loop *event_loop,
    struct aws_overlapped *overlapped);
static void s_read_end_report_error_task(void *user_data, aws_task_status status);
static void s_read_end_finish_closing_task(void *read_end, aws_task_status task_status);
static void s_write_end_on_write_completion(struct aws_event_loop *event_loop, struct aws_overlapped *overlapped);
static void s_write_end_finish_closing_task(void *write_end, aws_task_status task_status);

/* Translate Windows errors into aws_pipe errors */
static int s_translate_windows_error(DWORD win_error) {
    switch (win_error) {
        case ERROR_INVALID_HANDLE:
            return AWS_IO_FILE_NOT_FOUND;
        case ERROR_BROKEN_PIPE:
            return AWS_IO_BROKEN_PIPE;
        default:
            return AWS_IO_SYS_CALL_FAILURE;
    }
}

static int s_raise_last_windows_error() {
    DWORD win_error = GetLastError();
    int aws_error = s_translate_windows_error(win_error);
    return aws_raise_error(aws_error);
}

static int s_create_random_pipe_name(char *buffer, size_t buffer_size) {
    /* Name should be unique per-machine.
     * Use Windows UUID functions as source of randomness. */
    UUID uuid;
    RPC_STATUS uuid_status = UuidCreateSequential(&uuid);
    if (uuid_status == RPC_S_UUID_NO_ADDRESS) {
        return aws_raise_error(AWS_IO_SYS_CALL_FAILURE);
    }

    RPC_CSTR uuid_string;
    uuid_status = UuidToStringA(&uuid, &uuid_string);
    if (uuid_status != RPC_S_OK) {
        return aws_raise_error(AWS_IO_SYS_CALL_FAILURE);
    }

    snprintf(buffer, buffer_size, "\\\\.\\pipe\\aws_pipe_%s", uuid_string);

    RpcStringFreeA(&uuid_string);
    return AWS_OP_SUCCESS;
}

int aws_pipe_open(
    struct aws_pipe_read_end *read_end,
    struct aws_event_loop *read_end_event_loop,
    struct aws_pipe_write_end *write_end,
    struct aws_event_loop *write_end_event_loop,
    struct aws_allocator *allocator) {

    assert(read_end);
    assert(read_end_event_loop);
    assert(write_end);
    assert(write_end_event_loop);
    assert(allocator);

    AWS_ZERO_STRUCT(write_end);
    AWS_ZERO_STRUCT(read_end);

    struct write_end_impl *write_impl = NULL;
    struct read_end_impl *read_impl = NULL;

    /* Setup write-end */
    write_impl = aws_mem_acquire(allocator, sizeof(struct write_end_impl));
    if (!write_impl) {
        goto clean_up;
    }

    AWS_ZERO_STRUCT(*write_impl);
    write_impl->alloc = allocator;
    write_impl->state = WRITE_END_STATE_OPEN;
    write_impl->handle.data.handle = INVALID_HANDLE_VALUE;
    aws_linked_list_init(&write_impl->write_list);

    /* Anonymous pipes don't support overlapped I/O so named pipes are used. Names must be unique system-wide.
     * We generate random names, but collisions are theoretically possible, so try several times before giving up. */
    char pipe_name[256];
    int tries = 0;
    while (true) {
        int err = s_create_random_pipe_name(pipe_name, sizeof(pipe_name));
        if (err) {
            goto clean_up;
        }

        const DWORD open_mode = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;

        const DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;

        write_impl->handle.data.handle = CreateNamedPipeA(
            pipe_name,
            open_mode,
            pipe_mode,
            1,                /*nMaxInstances*/
            PIPE_BUFFER_SIZE, /*nOutBufferSize*/
            PIPE_BUFFER_SIZE, /*nInBufferSize*/
            0,                /*nDefaultTimeout: 0 means default*/
            NULL);            /*lpSecurityAttributes: NULL means default */

        if (write_impl->handle.data.handle != INVALID_HANDLE_VALUE) {
            /* Success, break out of loop */
            break;
        }

        if (++tries >= PIPE_UNIQUE_NAME_MAX_TRIES) {
            s_raise_last_windows_error();
            goto clean_up;
        }
    }

    int err = aws_event_loop_connect_handle_to_io_completion_port(write_end_event_loop, &write_impl->handle);
    if (err) {
        goto clean_up;
    }

    write_impl->event_loop = write_end_event_loop;

    /* Setup read-end */
    read_impl = aws_mem_acquire(allocator, sizeof(struct read_end_impl));
    if (!read_impl) {
        goto clean_up;
    }

    AWS_ZERO_STRUCT(*read_impl);
    read_impl->alloc = allocator;
    read_impl->state = READ_END_STATE_OPEN;
    read_impl->handle.data.handle = INVALID_HANDLE_VALUE;
    aws_overlapped_init(&read_impl->overlapped, s_read_end_on_zero_byte_read_completion, read_end);

    read_impl->handle.data.handle = CreateFileA(
        pipe_name,     /*lpFileName*/
        GENERIC_READ,  /*dwDesiredAccess*/
        0,             /*dwShareMode: 0 prevents acess by external processes*/
        NULL,          /*lpSecurityAttributes: NULL prevents inheritance by child processes*/
        OPEN_EXISTING, /*dwCreationDisposition*/
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, /*dwFlagsAndAttributes*/
        NULL);                                        /*hTemplateFile: ignored when opening existing file*/

    if (read_impl->handle.data.handle == INVALID_HANDLE_VALUE) {
        s_raise_last_windows_error();
        goto clean_up;
    }

    err = aws_event_loop_connect_handle_to_io_completion_port(read_end_event_loop, &read_impl->handle);
    if (err) {
        goto clean_up;
    }

    read_impl->event_loop = read_end_event_loop;

    /* Success */
    write_end->impl_data = write_impl;
    read_end->impl_data = read_impl;
    return AWS_OP_SUCCESS;

clean_up:
    if (write_impl) {
        if (write_impl->event_loop) {
            err = aws_event_loop_disconnect_handle_from_io_completion_port(write_impl->event_loop, &write_impl->handle);
        }

        if (write_impl->handle.data.handle != INVALID_HANDLE_VALUE) {
            CloseHandle(write_impl->handle.data.handle);
        }

        aws_mem_release(allocator, write_impl);
        write_impl = NULL;
    }

    if (read_impl) {
        if (read_impl->event_loop) {
            aws_event_loop_disconnect_handle_from_io_completion_port(read_impl->event_loop, &read_impl->handle);
        }

        if (read_impl->handle.data.handle != INVALID_HANDLE_VALUE) {
            CloseHandle(read_impl->handle.data.handle);
        }

        aws_mem_release(allocator, read_impl);
        read_impl = NULL;
    }

    return AWS_OP_ERR;
}

struct aws_event_loop *aws_pipe_get_read_end_event_loop(const struct aws_pipe_read_end *read_end) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);

    return read_impl->event_loop;
}

struct aws_event_loop *aws_pipe_get_write_end_event_loop(const struct aws_pipe_write_end *write_end) {
    struct write_end_impl *write_impl = write_end->impl_data;
    assert(write_impl);

    return write_impl->event_loop;
}

int aws_pipe_close_read_end(
    struct aws_pipe_read_end *read_end,
    aws_pipe_on_read_end_closed_fn *on_closed,
    void *user_data) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);

    if (read_impl->state == READ_END_STATE_CLOSING) {
        return aws_raise_error(AWS_ERROR_UNKNOWN); // TODO: AWS_ERROR_IO_ALREADY_CLOSING
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_MUST_RUN_ON_EVENT_LOOP_THREAD);
    }

    read_impl->state = READ_END_STATE_CLOSING;
    read_impl->on_closed_user_callback = on_closed;
    read_impl->on_closed_user_data = user_data;

    CloseHandle(read_impl->handle.data.handle);

    /* Can't finish clean up until all async operations complete.
     *
     * If there are any async operations pending, s_read_end_complete_async_operation() will finish cleaning up when
     * the operation completes. If a zero-byte-read is pending, it will complete soon due to the handle being closed.
     *
     * If no async operations are pending, we schedule a task to clean up, even though we could clean up immediately.
     * We do this because it's weird to invoke user callbacks before the function that sets them can return. */
    if (!read_impl->is_async_operation_pending) {
        struct aws_task task;
        task.fn = s_read_end_finish_closing_task;
        task.arg = read_end;
        aws_event_loop_schedule_task_now(read_impl->event_loop, &task);
        // TODO: wtf if this fails
    }

    return AWS_OP_SUCCESS;
}

static void s_read_end_finish_closing(struct aws_pipe_read_end *read_end) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl->state == READ_END_STATE_CLOSING);
    assert(!read_impl->is_async_operation_pending);
    assert(aws_event_loop_thread_is_callers_thread(read_impl->event_loop));

    /* Save off callback so we can invoke it last */
    aws_pipe_on_read_end_closed_fn *on_closed_user_callback = read_impl->on_closed_user_callback;
    void *on_closed_user_data = read_impl->on_closed_user_data;

    aws_event_loop_disconnect_handle_from_io_completion_port(read_impl->event_loop, &read_impl->handle);

    CloseHandle(read_impl->handle.data.handle);

    aws_mem_release(read_impl->alloc, read_impl);
    AWS_ZERO_STRUCT(*read_end);

    if (on_closed_user_callback) {
        on_closed_user_callback(read_end, on_closed_user_data);
    }
}

static void s_read_end_finish_closing_task(void *read_end, aws_task_status task_status) {
    (void)task_status;
    s_read_end_finish_closing(read_end);
}

/* Detect events on the pipe by kicking off an async zero-byte-read.
 * When the pipe becomes readable or an error occurs, the read will
 * complete and we will report the event. */
static void s_read_end_request_async_monitoring(struct aws_pipe_read_end *read_end, int request_reason) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);

    /* We only do async monitoring while user is subscribed, but not if we've
     * reported an error and moved into the SUBSCRIBE_ERROR state */
    bool async_monitoring_allowed =
        read_impl->state == READ_END_STATE_SUBSCRIBING || read_impl->state == READ_END_STATE_SUBSCRIBED;
    if (!async_monitoring_allowed) {
        return;
    }

    /* We can only have one monitoring operation active at a time. Save off
     * the reason for the request. When the current operation completes,
     * if this reason is still valid, we'll re-launch async monitoring */
    if (read_impl->is_async_operation_pending) {
        read_impl->monitoring_request_reasons |= request_reason;
        return;
    }

    assert(read_impl->error_events_to_report == 0);

    read_impl->monitoring_request_reasons = 0;
    read_impl->is_async_operation_pending = true;
    read_impl->state = READ_END_STATE_SUBSCRIBED;

    /* aws_overlapped must be reset before each use */
    aws_overlapped_reset(&read_impl->overlapped);

    int fake_buffer;
    bool success = ReadFile(
        read_impl->handle.data.handle,
        &fake_buffer,
        0,    /*nNumberOfBytesToRead*/
        NULL, /*lpNumberOfBytesRead: NULL for an overlapped operation*/
        &read_impl->overlapped.overlapped);

    if (success || (GetLastError() == ERROR_IO_PENDING)) {
        /* Success launching zero-byte-read, aka async monitoring operation */
        return;
    }

    /* User is subscribed for IO events and expects to be notified of
     * errors via the event callback. We schedule this as a task so
     * the callback doesn't happen before the user expects it.
     * We also set the state to SUBSCRIBE_ERROR so we don't keep trying
     * to monitor the file. */
    read_impl->state = READ_END_STATE_SUBSCRIBE_ERROR;
    read_impl->error_events_to_report = AWS_IO_EVENT_TYPE_ERROR; // TODO: translate errors more specifically

    struct aws_task task;
    task.fn = s_read_end_report_error_task;
    task.arg = read_end;

    aws_event_loop_schedule_task_now(read_impl->event_loop, &task);
    // TODO: wtf to do if this fails
}

/* Common functionality that needs to run after completion of any async task on the read-end */
static void s_read_end_complete_async_operation(struct aws_pipe_read_end *read_end) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);
    assert(read_impl->is_async_operation_pending);

    read_impl->is_async_operation_pending = false;

    switch (read_impl->state) {
        case READ_END_STATE_CLOSING:
            s_read_end_finish_closing(read_end);
            return;

        case READ_END_STATE_SUBSCRIBING:
        case READ_END_STATE_SUBSCRIBED:
            /* Check if there's a reason to relaunch async monitoring */
            if (read_impl->monitoring_request_reasons != 0) {
                s_read_end_request_async_monitoring(read_end, read_impl->monitoring_request_reasons);
            }
            return;
    }
}

static void s_read_end_report_error_task(void *user_data, aws_task_status status) {
    (void)status; /* Do same work whether or not this is a "cancelled" task */

    struct aws_pipe_read_end *read_end = user_data;
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);
    assert(read_impl->is_async_operation_pending);

    /* Only report the error if we're still in the SUBSCRIBE_ERROR state.
     * If the user closed or unsubscribed since this task was queued, then
     * we'd be in a different state. */
    if (read_impl->state == READ_END_STATE_SUBSCRIBE_ERROR) {
        assert(read_impl->error_events_to_report != 0);

        if (read_impl->on_read_event_user_callback) {
            read_impl->on_read_event_user_callback(
                read_end, read_impl->error_events_to_report, read_impl->on_read_event_user_data);
        }
    }

    s_read_end_complete_async_operation(read_end);
}

static void s_read_end_on_zero_byte_read_completion(
    struct aws_event_loop *event_loop,
    struct aws_overlapped *overlapped) {

    (void)event_loop;
    struct aws_pipe_read_end *read_end = overlapped->user_data;
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);

    /* TODO: explain why only this one state works */
    if (read_impl->state == READ_END_STATE_SUBSCRIBED) {
        int events;
        if (overlapped->overlapped.Internal == 0) {
            events = AWS_IO_EVENT_TYPE_READABLE;

            /* Clear out the "need more data" reason to restart zero-byte-read,
             * since we're about to tell the user that the pipe is readable.
             * If the user consumes all the data, the reason will get set again
             * and async-monitoring will be realaunched at the end of
             * s_read_end_complete_async_operation()  */
            read_impl->monitoring_request_reasons &= ~MONITORING_BECAUSE_WAITING_FOR_DATA;
        } else {
            /* Move pipe to SUBSCRIBE_ERROR state so we don't keep monitoring */
            read_impl->state = READ_END_STATE_SUBSCRIBE_ERROR;
            events = AWS_IO_EVENT_TYPE_ERROR; // TODO: what error codes do we get if the pipe is closed?
        }

        /* TODO: Explain how responses like close() and need more data will
         * be queued, and handled when we end the async operation */
        if (read_impl->on_read_event_user_callback) {
            read_impl->on_read_event_user_callback(read_end, events, read_impl->on_read_event_user_data);
        }
    }

    s_read_end_complete_async_operation(read_end);
}

int aws_pipe_subscribe_to_read_events(
    struct aws_pipe_read_end *read_end,
    aws_pipe_on_read_event_fn *on_read_event,
    void *user_data) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);

    if (read_impl->state != READ_END_STATE_OPEN) {
        return aws_raise_error(AWS_ERROR_UNKNOWN); // TODO more granual errors: closing vs already-subscribed
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return AWS_ERROR_IO_MUST_RUN_ON_EVENT_LOOP_THREAD;
    }

    read_impl->state = READ_END_STATE_SUBSCRIBING;
    read_impl->on_read_event_user_callback = on_read_event;
    read_impl->on_read_event_user_data = user_data;

    s_read_end_request_async_monitoring(read_end, MONITORING_BECAUSE_SUBSCRIBING);

    return AWS_OP_SUCCESS;
}

int aws_pipe_unsubscribe_from_read_events(struct aws_pipe_read_end *read_end) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);

    if (read_impl->state < READ_END_STATE_SUBSCRIBING) {
        return aws_raise_error(AWS_ERROR_UNKNOWN); // TODO NOT_SUBSCRIBED
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return AWS_ERROR_IO_MUST_RUN_ON_EVENT_LOOP_THREAD;
    }

    read_impl->state = READ_END_STATE_OPEN;
    read_impl->on_read_event_user_callback = NULL;
    read_impl->on_read_event_user_data = NULL;
    read_impl->monitoring_request_reasons = 0;
    read_impl->error_events_to_report = 0;

    /* If there's a chance the zero-byte-read is pending, cancel it.
     * s_read_end_on_zero_byte_read_completion() will see status code
     * ERROR_OPERATION_ABORTED, but won't pass the event to the user
     * because we're not in the SUBSCRIBED state anymore. */
    if (read_impl->is_async_operation_pending) {
        CancelIo(read_impl->handle.data.handle);
    }

    return AWS_OP_SUCCESS;
}

int aws_pipe_read(struct aws_pipe_read_end *read_end, uint8_t *dst, size_t dst_size, size_t *amount_read) {
    struct read_end_impl *read_impl = read_end->impl_data;
    assert(read_impl);
    assert(dst);

    if (amount_read) {
        *amount_read = 0;
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_MUST_RUN_ON_EVENT_LOOP_THREAD);
    }

    // TODO: what if state is closing?

    if (dst_size == 0) {
        return AWS_OP_SUCCESS;
    }

    /* ReadFile() will be called in synchronous mode and would block
     * indefinitely if it asked for more bytes than are currently available.
     * Therefore, peek at the available bytes before performing the actual read. */
    DWORD bytes_available = 0;
    bool peek_success = PeekNamedPipe(
        read_impl->handle.data.handle,
        NULL,             /*lpBuffer: NULL so peek doesn't actually copy data */
        0,                /*nBufferSize*/
        NULL,             /*lpBytesRead*/
        &bytes_available, /*lpTotalBytesAvail*/
        NULL);            /*lpBytesLeftThisMessage: doesn't apply to byte-type pipes*/

    /* Operation failed. Request async monitoring so user is informed via aws_pipe_on_read_event_fn of handle error. */
    if (!peek_success) {
        s_read_end_request_async_monitoring(read_end, MONITORING_BECAUSE_ERROR_SUSPECTED);
        return s_raise_last_windows_error();
    }

    /* No data available. Request async monitoring so user is notified when data becomes available. */
    if (bytes_available == 0) {
        s_read_end_request_async_monitoring(read_end, MONITORING_BECAUSE_WAITING_FOR_DATA);
        return aws_raise_error(AWS_IO_READ_WOULD_BLOCK);
    }

    DWORD bytes_read = 0;
    DWORD bytes_to_read = dst_size > bytes_available ? bytes_available : (DWORD)dst_size;
    bool read_success = ReadFile(
        read_impl->handle.data.handle,
        dst,           /*lpBuffer*/
        bytes_to_read, /*nNumberOfBytesToRead*/
        &bytes_read,   /*lpNumberOfBytesRead*/
        NULL);         /*lpOverlapped: NULL so read is synchronous*/

    /* Operation failed. Request async monitoring so user is informed via aws_pipe_on_read_event_fn of handle error. */
    if (!read_success) {
        s_read_end_request_async_monitoring(read_end, MONITORING_BECAUSE_ERROR_SUSPECTED);
        return s_raise_last_windows_error();
    }

    if (bytes_read < dst_size) {
        /* If we weren't able to read as many bytes as the user requested, that's ok.
         * Request async monitoring so we can alert the user when more data arrives */
        s_read_end_request_async_monitoring(read_end, MONITORING_BECAUSE_WAITING_FOR_DATA);
    }

    if (amount_read) {
        *amount_read = bytes_read;
    }

    return AWS_OP_SUCCESS;
}

int aws_pipe_close_write_end(
    struct aws_pipe_write_end *write_end,
    aws_pipe_on_write_end_closed_fn *on_closed,
    void *user_data) {

    struct write_end_impl *write_impl = write_end->impl_data;
    assert(write_impl); // TOOD: replace impl asserts in public functions with AWS_ERROR_IO_NOT_OPEN?

    if (write_impl->state == WRITE_END_STATE_CLOSING) {
        return aws_raise_error(AWS_ERROR_UNKNOWN); // TODO: alreayd closing
    }

    if (!aws_event_loop_thread_is_callers_thread(write_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_MUST_RUN_ON_EVENT_LOOP_THREAD);
    }

    write_impl->state = WRITE_END_STATE_CLOSING;
    write_impl->on_closed_user_fn = on_closed;
    write_impl->on_closed_user_data = user_data;

    CloseHandle(write_impl->handle.data.handle);

    /* Can't clean up until all async operations complete.
     *
     * If there are pending writes, closing the handle will cause them to complete with status code ERROR_BROKEN_PIPE,
     * and s_write_end_on_write_completion() will finish cleaning up the pipe when the last write completes.
     *
     * If there are no pending writes, schedule the shutdown to complete on the event-loop thread. Though we could clean
     * up immediately, it would be weird to invoke user callbacks before the function that sets them can return. */
    if (!aws_linked_list_empty(&write_impl->write_list)) {
        /* Cancel any pending writes. s_write_end_on_write_completion() will
         * see status code ERROR_OPERATION_ABORTED. The pipe will finish
         * closing when the last write operation completes */
        CancelIo(write_impl->handle.data.handle);

    } else {
        /* Even though we could close immediately, schedule the shutdown to complete on the event-loop thread.
         * We do this because it's weird to invoke user callbacks before the function that sets them can return. */
        struct aws_task task;
        task.fn = s_write_end_finish_closing_task;
        task.arg = write_end;
        aws_event_loop_schedule_task_now(write_impl->event_loop, &task);
        // TODO WTF IF THIS FAILS
    }

    return AWS_OP_SUCCESS;
}

static void s_write_end_finish_closing(struct aws_pipe_write_end *write_end) {
    struct write_end_impl *write_impl = write_end->impl_data;
    assert(write_impl);
    assert(write_impl->state == WRITE_END_STATE_CLOSING);
    assert(aws_linked_list_empty(&write_impl->write_list));
    assert(aws_event_loop_thread_is_callers_thread(write_impl->event_loop));

    /* Save off callback so we can invoke it last */
    aws_pipe_on_write_end_closed_fn *on_closed_user_fn = write_impl->on_closed_user_fn;
    void *on_closed_user_data = write_impl->on_closed_user_data;

    aws_event_loop_disconnect_handle_from_io_completion_port(write_impl->event_loop, &write_impl->handle);

    aws_mem_release(write_impl->alloc, write_impl);
    AWS_ZERO_STRUCT(*write_end);

    if (on_closed_user_fn) {
        on_closed_user_fn(write_end, on_closed_user_data);
    }
}

static void s_write_end_finish_closing_task(void *write_end, aws_task_status task_status) {
    (void)task_status;
    s_write_end_finish_closing(write_end); // TODO: rename "clean_up" instead of "close"
}

int aws_pipe_write(
    struct aws_pipe_write_end *write_end,
    const uint8_t *src,
    size_t src_size,
    aws_pipe_on_write_complete_fn *on_complete,
    void *user_data) {

    struct write_end_impl *write_impl = write_end->impl_data;
    assert(write_impl);

    if (write_impl->state != WRITE_END_STATE_OPEN) {
        return aws_raise_error(AWS_ERROR_UNKNOWN); // TODO: must be open
    }

    if (!aws_event_loop_thread_is_callers_thread(write_impl->event_loop)) {
        return AWS_ERROR_IO_MUST_RUN_ON_EVENT_LOOP_THREAD;
    }

    if (src_size > MAXDWORD) {
        return AWS_ERROR_UNKNOWN; // TODO: AWS_ERROR_SIZE_EXCEEDS_MAX
    }

    DWORD num_bytes_to_write = (DWORD)src_size;

    struct write_request *write = aws_mem_acquire(write_impl->alloc, sizeof(struct write_request));
    if (!write) {
        return AWS_OP_ERR;
    }

    AWS_ZERO_STRUCT(*write);
    write->user_callback = on_complete;
    write->user_data = user_data;
    aws_overlapped_init(&write->overlapped, s_write_end_on_write_completion, write_end);

    bool write_success = WriteFile(
        write_impl->handle.data.handle, /*hFile*/
        src,                            /*lpBuffer*/
        num_bytes_to_write,             /*nNumberOfBytesToWrite*/
        NULL,                           /*lpNumberOfBytesWritten*/
        &write->overlapped.overlapped); /*lpOverlapped*/

    /* Overlapped WriteFile() calls may succeed immediately, or they may queue the work. In either of these cases, IOCP
     * on the event-loop will alert us when the operation completes and we'll invoke user callbacks then. */
    if (!write_success && GetLastError() != ERROR_IO_PENDING) {
        aws_mem_release(write_impl->alloc, write);
        return s_raise_last_windows_error();
    }

    aws_linked_list_push_back(&write_impl->write_list, &write->list_node);
    return AWS_OP_SUCCESS;
}

void s_write_end_on_write_completion(struct aws_event_loop *event_loop, struct aws_overlapped *overlapped) {
    (void)event_loop;

    struct aws_pipe_write_end *write_end = overlapped->user_data;
    struct write_end_impl *write_impl = write_end->impl_data;
    assert(write_impl);

    struct write_request *write_request = AWS_CONTAINER_OF(overlapped, struct write_request, overlapped);
    assert(write_request);

    /* Report outcome to user */
    if (write_request->user_callback) {
        int write_result = AWS_ERROR_SUCCESS;
        if (overlapped->overlapped.Internal != 0) {
            write_result = s_translate_windows_error((DWORD)overlapped->overlapped.Internal);
        }

        write_request->user_callback(write_end, write_result, write_request->user_data);
    }

    /* Clean up write-request*/
    aws_linked_list_remove(&write_request->list_node);
    aws_mem_release(write_impl->alloc, write_request);

    /* If pipe is closing, and this was the last pending write request, finish closing pipe. */
    if (write_impl->state == WRITE_END_STATE_CLOSING && aws_linked_list_empty(&write_impl->write_list)) {

        s_write_end_finish_closing(write_end);
    }
}