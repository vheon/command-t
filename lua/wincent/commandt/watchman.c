/**
 * SPDX-FileCopyrightText: Copyright 2014-present Greg Hurrell and contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "watchman.h"

#include "str.h"
#include "xmalloc.h" /* for xmalloc() */

// TODO: mark most of these functions as static (internal only)

#include <fcntl.h> /* for F_GETFL, F_SETFL, O_NONBLOCK, fcntl() */
#include <stdint.h> /* for uint8_t */
#include <stdlib.h> /* for abort(), free() */
#include <string.h> /* for memset(), strncpy() */
#include <sys/errno.h> /* for errno */
#include <sys/socket.h> /* for AF_LOCAL, recv(), MSG_PEEK */
#include <sys/un.h> /* for sockaddr_un */
#include <unistd.h> /* for close() */

typedef struct {
    uint8_t *data;  // payload
    size_t cap;     // total capacity
    size_t len;     // current length
} watchman_payload_t;

// TODO: delete me
#if 0
// Forward declarations:
VALUE watchman_load(char **ptr, char *end);
void watchman_dump(watchman_payload_t *w, VALUE serializable);
#endif

#define WATCHMAN_DEFAULT_STORAGE 4096

#define WATCHMAN_BINARY_MARKER   "\x00\x01"
#define WATCHMAN_ARRAY_MARKER    0x00
#define WATCHMAN_HASH_MARKER     0x01
#define WATCHMAN_STRING_MARKER   0x02
#define WATCHMAN_INT8_MARKER     0x03
#define WATCHMAN_INT16_MARKER    0x04
#define WATCHMAN_INT32_MARKER    0x05
#define WATCHMAN_INT64_MARKER    0x06
#define WATCHMAN_FLOAT_MARKER    0x07
#define WATCHMAN_TRUE            0x08
#define WATCHMAN_FALSE           0x09
#define WATCHMAN_NIL             0x0a
#define WATCHMAN_TEMPLATE_MARKER 0x0b
#define WATCHMAN_SKIP_MARKER     0x0c

#define WATCHMAN_HEADER \
        WATCHMAN_BINARY_MARKER \
        "\x06" \
        "\x00\x00\x00\x00\x00\x00\x00\x00"

// How far we have to look to figure out the size of the PDU header.
#define WATCHMAN_SNIFF_BUFFER_SIZE sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t)

// How far we have to peek, at most, to figure out the size of the PDU itself.
#define WATCHMAN_PEEK_BUFFER_SIZE \
    sizeof(WATCHMAN_BINARY_MARKER) - 1 + \
    sizeof(WATCHMAN_INT64_MARKER) + \
    sizeof(int64_t)

static const char watchman_array_marker  = WATCHMAN_ARRAY_MARKER;
static const char watchman_hash_marker   = WATCHMAN_HASH_MARKER;
static const char watchman_string_marker = WATCHMAN_STRING_MARKER;
static const char watchman_true          = WATCHMAN_TRUE;
static const char watchman_false         = WATCHMAN_FALSE;
static const char watchman_nil           = WATCHMAN_NIL;

/**
 * Appends `len` bytes, starting at `data`, to the watchman_payload_t struct `w`
 *
 * Will attempt to reallocate the underlying storage if it is not sufficient.
 */
void watchman_append(watchman_payload_t *w, const char *data, size_t len) {
    if (w->len + len > w->cap) {
        w->cap += w->len + WATCHMAN_DEFAULT_STORAGE;
        xrealloc(w->data, sizeof(uint8_t) * w->cap);
    }
    memcpy(w->data + w->len, data, len);
    w->len += len;
}

/**
 * Allocate a new watchman_payload_t struct
 *
 * The struct has a small amount of extra capacity preallocated, and a blank
 * header that can be filled in later to describe the PDU.
 */
watchman_payload_t *watchman_init() {
    watchman_payload_t *w = xmalloc(sizeof(watchman_payload_t));
    w->cap = WATCHMAN_DEFAULT_STORAGE;
    w->len = 0;
    w->data = xcalloc(WATCHMAN_DEFAULT_STORAGE, sizeof(uint8_t));

    watchman_append(w, WATCHMAN_HEADER, sizeof(WATCHMAN_HEADER) - 1);
    return w;
}

/**
 * Free a watchman_payload_t struct `w` that was previously allocated with
 * `watchman_init`
 */
void watchman_free(watchman_payload_t *w) {
    free(w->data);
    free(w);
}

/**
 * Encodes and appends the integer `num` to `w`
 */
void watchman_dump_int(watchman_payload_t *w, int64_t num) {
    char encoded[1 + sizeof(int64_t)];

    if (num == (int8_t)num) {
        encoded[0] = WATCHMAN_INT8_MARKER;
        encoded[1] = (int8_t)num;
        watchman_append(w, encoded, 1 + sizeof(int8_t));
    } else if (num == (int16_t)num) {
        encoded[0] = WATCHMAN_INT16_MARKER;
        *(int16_t *)(encoded + 1) = (int16_t)num;
        watchman_append(w, encoded, 1 + sizeof(int16_t));
    } else if (num == (int32_t)num) {
        encoded[0] = WATCHMAN_INT32_MARKER;
        *(int32_t *)(encoded + 1) = (int32_t)num;
        watchman_append(w, encoded, 1 + sizeof(int32_t));
    } else {
        encoded[0] = WATCHMAN_INT64_MARKER;
        *(int64_t *)(encoded + 1) = (int64_t)num;
        watchman_append(w, encoded, 1 + sizeof(int64_t));
    }
}

/**
 * Encodes and appends the string `string` to `w`
 */
void watchman_dump_string(watchman_payload_t *w, const char *string, size_t length) {
    watchman_append(w, &watchman_string_marker, sizeof(watchman_string_marker));
    watchman_dump_int(w, length);
    watchman_append(w, string, length);
}

/**
 * Encodes and appends the double `num` to `w`
 */
void watchman_dump_double(watchman_payload_t *w, double num) {
    char encoded[1 + sizeof(double)];
    encoded[0] = WATCHMAN_FLOAT_MARKER;
    *(double *)(encoded + 1) = num;
    watchman_append(w, encoded, sizeof(encoded));
}

#if 0

/**
 * Encodes and appends the array `array` to `w`
 */
void watchman_dump_array(watchman_payload_t *w, VALUE array) {
    long i;
    watchman_append(w, &watchman_array_marker, sizeof(watchman_array_marker));
    watchman_dump_int(w, RARRAY_LEN(array));
    for (i = 0; i < RARRAY_LEN(array); i++) {
        watchman_dump(w, rb_ary_entry(array, i));
    }
}

/**
 * Helper method that encodes and appends a key/value pair (`key`, `value`) from
 * a hash to the watchman_payload_t struct passed in via `data`
 */
int watchman_dump_hash_iterator(const char *key_ptr, size_t key_len, VALUE value, VALUE data) {
    watchman_payload_t *w = (watchman_payload_t *)data;
    watchman_dump_string(w, key_ptr, key_len);
    watchman_dump(w, value);
    return ST_CONTINUE;
}

/**
 * Encodes and appends the hash `hash` to `w`
 */
void watchman_dump_hash(watchman_payload_t *w, VALUE hash) {
    watchman_append(w, &watchman_hash_marker, sizeof(watchman_hash_marker));
    watchman_dump_int(w, RHASH_SIZE(hash));
    rb_hash_foreach(hash, watchman_dump_hash_iterator, (VALUE)w);
}

/**
 * Encodes and appends the serialized Ruby object `serializable` to `w`
 *
 * Examples of serializable objects include arrays, hashes, strings, numbers
 * (integers, floats), booleans, and nil.
 */
void watchman_dump(watchman_payload_t *w, VALUE serializable) {
    switch (TYPE(serializable)) {
        case T_ARRAY:
            return watchman_dump_array(w, serializable);
        case T_HASH:
            return watchman_dump_hash(w, serializable);
        case T_STRING:
            return watchman_dump_string(w, RSTRING_PTR(serializable), RSTRING_LEN(serializable));
        case T_FIXNUM: // up to 63 bits
            return watchman_dump_int(w, FIX2LONG(serializable));
        case T_BIGNUM:
            return watchman_dump_int(w, NUM2LL(serializable));
        case T_FLOAT:
            return watchman_dump_double(w, NUM2DBL(serializable));
        case T_TRUE:
            return watchman_append(w, &watchman_true, sizeof(watchman_true));
        case T_FALSE:
            return watchman_append(w, &watchman_false, sizeof(watchman_false));
        case T_NIL:
            return watchman_append(w, &watchman_nil, sizeof(watchman_nil));
        default:
            rb_raise(rb_eTypeError, "unsupported type");
    }
}
#endif

/**
 * Extract and return the int encoded at `ptr`
 *
 * Moves `ptr` past the extracted int.
 *
 * Will raise an ArgumentError if extracting the int would take us beyond the
 * end of the buffer indicated by `end`, or if there is no int encoded at `ptr`.
 *
 * @returns The extracted int
 */
int64_t watchman_load_int(char **ptr, char *end) {
    char *val_ptr = *ptr + sizeof(int8_t);
    int64_t val = 0;

    if (val_ptr >= end) {
        abort(); // Insufficient int storage.
    }

    switch (*ptr[0]) {
        case WATCHMAN_INT8_MARKER:
            if (val_ptr + sizeof(int8_t) > end) {
                abort(); // Overrun extracting int8_t.
            }
            val = *(int8_t *)val_ptr;
            *ptr = val_ptr + sizeof(int8_t);
            break;
        case WATCHMAN_INT16_MARKER:
            if (val_ptr + sizeof(int16_t) > end) {
                abort(); // Overrun extracting int16_t.
            }
            val = *(int16_t *)val_ptr;
            *ptr = val_ptr + sizeof(int16_t);
            break;
        case WATCHMAN_INT32_MARKER:
            if (val_ptr + sizeof(int32_t) > end) {
                abort(); // Overrun extracting int32_t.
            }
            val = *(int32_t *)val_ptr;
            *ptr = val_ptr + sizeof(int32_t);
            break;
        case WATCHMAN_INT64_MARKER:
            if (val_ptr + sizeof(int64_t) > end) {
                abort(); // Overrun extracting int64_t.
            }
            val = *(int64_t *)val_ptr;
            *ptr = val_ptr + sizeof(int64_t);
            break;
        default:
            abort(); // Bad integer marker.
            break;
    }

    return val;
}

/**
 * Reads and returns a string encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
str_t *watchman_load_string(char **ptr, char *end) {
    int64_t len;
    if (*ptr >= end) {
        abort(); // Unexpected end of input.
    }

    if (*ptr[0] != WATCHMAN_STRING_MARKER) {
        abort(); // Not a number.
    }

    *ptr += sizeof(int8_t);
    if (*ptr >= end) {
        abort(); // Invalid string header.
    }

    len = watchman_load_int(ptr, end);
    if (len == 0) { // Special case for zero-length strings.
        return str_new_copy("", 0);
    } else if (*ptr + len > end) {
        abort(); // Insufficient string storage.
    }

    str_t *string = str_new_copy(*ptr, len);
    *ptr += len;
    return string;
}

/**
 * Reads and returns a double encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
double watchman_load_double(char **ptr, char *end) {
    double val;
    *ptr += sizeof(int8_t); // Caller has already verified the marker.
    if (*ptr + sizeof(double) > end) {
        abort(); // Insufficient double storage.
    }
    val = *(double *)*ptr;
    *ptr += sizeof(double);
    return val;
}

#if 0

/**
 * Reads and returns an array encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load_array(char **ptr, char *end) {
    int64_t count, i;
    VALUE array;

    if (*ptr >= end) {
        abort(); // Unexpected end of input.
    }

    // Verify and consume marker.
    if (*ptr[0] != WATCHMAN_ARRAY_MARKER) {
        abort(); // Not an array.
    }
    *ptr += sizeof(int8_t);

    // Expect a count.
    if (*ptr + sizeof(int8_t) * 2 > end) {
        abort(); // Incomplete array header.
    }

    int64_t count = watchman_load_int(ptr, end);

    array = rb_ary_new2(count);

    for (i = 0; i < count; i++) {
        rb_ary_push(array, watchman_load(ptr, end));
    }

    return array;
}

/**
 * Reads and returns a hash encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load_hash(char **ptr, char *end) {
    int64_t count, i;
    VALUE hash, key, value;

    *ptr += sizeof(int8_t); // caller has already verified the marker

    // expect a count
    if (*ptr + sizeof(int8_t) * 2 > end) {
        rb_raise(rb_eArgError, "incomplete hash header");
    }
    count = watchman_load_int(ptr, end);

    hash = rb_hash_new();

    for (i = 0; i < count; i++) {
        key = watchman_load_string(ptr, end);
        value = watchman_load(ptr, end);
        rb_hash_aset(hash, key, value);
    }

    return hash;
}

/**
 * Reads and returns an object encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load(char **ptr, char *end) {
    if (*ptr >= end) {
        abort(); // Unexpected end of input.
    }

    switch (*ptr[0]) {
        case WATCHMAN_ARRAY_MARKER:
            return watchman_load_array(ptr, end);
        case WATCHMAN_HASH_MARKER:
            return watchman_load_hash(ptr, end);
        case WATCHMAN_STRING_MARKER:
            return watchman_load_string(ptr, end);
        case WATCHMAN_INT8_MARKER:
        case WATCHMAN_INT16_MARKER:
        case WATCHMAN_INT32_MARKER:
        case WATCHMAN_INT64_MARKER:
            return LL2NUM(watchman_load_int(ptr, end));
        case WATCHMAN_FLOAT_MARKER:
            return rb_float_new(watchman_load_double(ptr, end));
        case WATCHMAN_TRUE:
            *ptr += 1;
            return Qtrue;
        case WATCHMAN_FALSE:
            *ptr += 1;
            return Qfalse;
        case WATCHMAN_NIL:
            *ptr += 1;
            return Qnil;
        case WATCHMAN_TEMPLATE_MARKER:
        default:
            abort(); // Unsupported type.
    }

    return Qnil; // keep the compiler happy
}

/**
 * CommandT::Watchman::Utils.load(serialized)
 *
 * Converts the binary object, `serialized`, from the Watchman binary protocol
 * format into a normal Ruby object.
 */
VALUE CommandTWatchmanUtils_load(VALUE self, VALUE serialized) {
    char *ptr, *end;
    long len;
    uint64_t payload_size;
    VALUE loaded;
    serialized = StringValue(serialized);
    len = RSTRING_LEN(serialized);
    ptr = RSTRING_PTR(serialized);
    end = ptr + len;

    // expect at least the binary marker and a int8_t length counter
    if ((size_t)len < sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t) * 2) {
        rb_raise(rb_eArgError, "undersized header");
    }

    if (memcmp(ptr, WATCHMAN_BINARY_MARKER, sizeof(WATCHMAN_BINARY_MARKER) - 1)) {
        rb_raise(rb_eArgError, "missing binary marker");
    }

    // get size marker
    ptr += sizeof(WATCHMAN_BINARY_MARKER) - 1;
    payload_size = watchman_load_int(&ptr, end);
    if (!payload_size) {
        rb_raise(rb_eArgError, "empty payload");
    }

    // sanity check length
    if (ptr + payload_size != end) {
        rb_raise(
            rb_eArgError,
            "payload size mismatch (%lu)",
            (unsigned long)(end - (ptr + payload_size))
        );
    }

    loaded = watchman_load(&ptr, end);

    // one more sanity check
    if (ptr != end) {
        rb_raise(
            rb_eArgError,
            "payload termination mismatch (%lu)",
            (unsigned long)(end - ptr)
        );
    }

    return loaded;
}

/**
 * CommandT::Watchman::Utils.dump(serializable)
 *
 * Converts the Ruby object, `serializable`, into a binary string in the
 * Watchman binary protocol format.
 *
 * Examples of serializable objects include arrays, hashes, strings, numbers
 * (integers, floats), booleans, and nil.
 */
VALUE CommandTWatchmanUtils_dump(VALUE self, VALUE serializable) {
    uint64_t *len;
    VALUE serialized;
    watchman_payload_t *w = watchman_init();
    watchman_dump(w, serializable);

    // update header with final length information
    len = (uint64_t *)(w->data + sizeof(WATCHMAN_HEADER) - sizeof(uint64_t) - 1);
    *len = w->len - sizeof(WATCHMAN_HEADER) + 1;

    // prepare final return value
    serialized = rb_str_buf_new(w->len);
    rb_str_buf_cat(serialized, (const char*)w->data, w->len);
    watchman_free(w);
    return serialized;
}

/**
 * Helper method for raising a SystemCallError wrapping a lower-level error code
 * coming from the `errno` global variable.
 */
void watchman_raise_system_call_error(int number) {
    VALUE error = INT2FIX(number);
    rb_exc_raise(rb_class_new_instance(1, &error, rb_eSystemCallError));
}

/**
 * CommandT::Watchman::Utils.query(query, socket)
 *
 * Converts `query`, a Watchman query comprising Ruby objects, into the Watchman
 * binary protocol format, transmits it over socket, and unserializes and
 * returns the result.
 */
VALUE CommandTWatchmanUtils_query(VALUE self, VALUE query, VALUE socket) {
    char *payload;
    int fileno, flags;
    int8_t peek[WATCHMAN_PEEK_BUFFER_SIZE];
    int8_t sizes[] = { 0, 0, 0, 1, 2, 4, 8 };
    int8_t sizes_idx;
    int8_t *pdu_size_ptr;
    int64_t payload_size;
    long query_len;
    ssize_t peek_size, sent, received;
    void *buffer;
    VALUE loaded, serialized;
    fileno = NUM2INT(rb_funcall(socket, rb_intern("fileno"), 0));

    // do blocking I/O to simplify the following logic
    flags = fcntl(fileno, F_GETFL);
    if (fcntl(fileno, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        rb_raise(rb_eRuntimeError, "unable to clear O_NONBLOCK flag");
    }

    // send the message
    serialized = CommandTWatchmanUtils_dump(self, query);
    query_len = RSTRING_LEN(serialized);
    sent = send(fileno, RSTRING_PTR(serialized), query_len, 0);
    if (sent == -1) {
        watchman_raise_system_call_error(errno);
    } else if (sent != query_len) {
        rb_raise(rb_eRuntimeError, "expected to send %ld bytes but sent %zd",
            query_len, sent);
    }

    // sniff to see how large the header is
    received = recv(fileno, peek, WATCHMAN_SNIFF_BUFFER_SIZE, MSG_PEEK | MSG_WAITALL);
    if (received == -1) {
        watchman_raise_system_call_error(errno);
    } else if (received != WATCHMAN_SNIFF_BUFFER_SIZE) {
        rb_raise(rb_eRuntimeError, "failed to sniff PDU header");
    }

    // peek at size of PDU
    sizes_idx = peek[sizeof(WATCHMAN_BINARY_MARKER) - 1];
    if (sizes_idx < WATCHMAN_INT8_MARKER || sizes_idx > WATCHMAN_INT64_MARKER) {
        rb_raise(rb_eRuntimeError, "bad PDU size marker");
    }
    peek_size = sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t) +
        sizes[sizes_idx];

    received = recv(fileno, peek, peek_size, MSG_PEEK);
    if (received == -1) {
        watchman_raise_system_call_error(errno);
    } else if (received != peek_size) {
        rb_raise(rb_eRuntimeError, "failed to peek at PDU header");
    }
    pdu_size_ptr = peek + sizeof(WATCHMAN_BINARY_MARKER) - sizeof(int8_t);
    payload_size =
        peek_size +
        watchman_load_int((char **)&pdu_size_ptr, (char *)peek + peek_size);

    // actually read the PDU
    buffer = xmalloc(payload_size);
    if (!buffer) {
        rb_raise(
            rb_eNoMemError,
            "failed to allocate %lld bytes",
            (long long int)payload_size
        );
    }
    received = recv(fileno, buffer, payload_size, MSG_WAITALL);
    if (received == -1) {
        watchman_raise_system_call_error(errno);
    } else if (received != payload_size) {
        rb_raise(rb_eRuntimeError, "failed to load PDU");
    }
    payload = (char *)buffer + peek_size;
    loaded = watchman_load(&payload, payload + payload_size);
    free(buffer);
    return loaded;
}

#endif

int commandt_watchman_connect(const char *socket_path) {
    int fd = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un) - 1);
    addr.sun_family = AF_LOCAL;

    // On macOS, `sun_path` is 104 bytes long... so good thing the socket path
    // is only: "/opt/homebrew/var/run/watchman/wincent-state/sock"...
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        return -1;
    }

    // Do blocking I/O to make logic simpler.
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return -1;
    }

    return fd;
}

int commandt_watchman_disconnect(int socket) {
    if (close(socket) == 0) {
        return 0;
    } else {
        return errno;
    }
}

watchman_query_result_t *commandt_watchman_query(
    const char *root,
    const char *relative_root,
    int socket
) {
    watchman_query_result_t *result = xmalloc(sizeof(watchman_query_result_t));

    // 1. send
    //   ["query", root, {"expression": ["type", "f"], "fields": ["name"]}]
    //   or
    //   ["query", root, {"expression": ["type", "f"], "fields": ["name"],
    //   "relative_root": relative_root}] (if we have that)
    // 2. extract "files"
    // 3. return NULL if "error"
    return result;
}

void commandt_watchman_query_result_free(watchman_query_result_t *result) {
    // TODO: free more stuff...
    free(result);
}

watchman_watch_project_result_t *commandt_watchman_watch_project(
    const char *root,
    int socket
) {
    watchman_watch_project_result_t *result =
        xmalloc(sizeof(watchman_watch_project_result_t));

    // Prepare the message.
    char *message = ""; // ["watch-project", root]
    ssize_t length = 0;

    // Send the message.
    ssize_t sent = send(socket, message, length, 0);
    if (sent == -1 || sent != length) {
        return NULL;
    }

    // Sniff to see how large the header is.
    int8_t peek[WATCHMAN_PEEK_BUFFER_SIZE];
    ssize_t received = recv(socket, peek, WATCHMAN_SNIFF_BUFFER_SIZE, MSG_PEEK | MSG_WAITALL);
    if (received == -1 || received != WATCHMAN_SNIFF_BUFFER_SIZE) {
        return NULL;
    }

    // Peek at size of PDU.
    int8_t sizes_idx = peek[sizeof(WATCHMAN_BINARY_MARKER) - 1];
    if (sizes_idx < WATCHMAN_INT8_MARKER || sizes_idx > WATCHMAN_INT64_MARKER) {
        return NULL;
    }
    int8_t sizes[] = {0, 0, 0, 1, 2, 4, 8};
    ssize_t peek_size = sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t) +
        sizes[sizes_idx];

    received = recv(socket, peek, peek_size, MSG_PEEK);
    if (received == -1 || received != peek_size) {
        return NULL;
    }
    int8_t *pdu_size_ptr = peek + sizeof(WATCHMAN_BINARY_MARKER) - sizeof(int8_t);
    int64_t payload_size =
        peek_size +
        watchman_load_int((char **)&pdu_size_ptr, (char *)peek + peek_size);

    // Actually read the PDU.
    char *buffer = xmalloc(payload_size);
    if (!buffer) {
        return NULL;
    }
    received = recv(socket, buffer, payload_size, MSG_WAITALL);
    if (received == -1 || received != payload_size) {
        // TODO free buffer
        return NULL;
    }
    char *payload = buffer + peek_size;
    // this would return a Ruby object.
    //loaded = watchman_load(&payload, payload + payload_size);
    free(buffer);

    // 2. extract "watch"
    // 3. extract "relative_path", if present
    // 4. return NULL if "error"

    return result;
}

void commandt_watchman_watch_project_result_free(
    watchman_watch_project_result_t *result
) {
    free((void *)result->watch);
    free((void *)result->relative_path);
    free(result);
}
