/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional PSN backend: guest handles, memory and callbacks stay in this host.
 * The registry lock may nest the backend lock, never the other way around.
 * Guest callbacks run after both locks have been released. */
#define _GNU_SOURCE
#include "psn_host.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define WEB_ERROR(n) ((int32_t)(UINT32_C(0x80552900) | (n)))
#define WEB_NOMEM WEB_ERROR(1)
#define WEB_INVALID WEB_ERROR(2)
#define WEB_NO_LIB WEB_ERROR(4)
#define WEB_NO_CONTEXT WEB_ERROR(5)
#define WEB_NO_REQUEST WEB_ERROR(6)
#define WEB_ABORTED WEB_ERROR(9)
#define WEB_NO_FILTER WEB_ERROR(11)
#define WEB_NO_CALLBACK WEB_ERROR(12)
#define WEB_NO_HANDLE WEB_ERROR(13)
#define WEB_BUSY WEB_ERROR(19)
#define WEB_AFTER_SEND WEB_ERROR(30)
#define WEB_TIMEOUT WEB_ERROR(31)
#define MAX_OBJECTS 256u
#define MAX_FILTER_PARAMS 32u
#define MAX_QUEUE_BYTES PSN_MAX_BODY

typedef struct {
    uint64_t length;
    const char *type;
    unsigned char reserved[16];
} GuestContent;
typedef struct {
    int32_t status;
    char *error;
    uint64_t error_size, response_size;
} GuestResponse;
typedef struct {
    char value[65];
} GuestDataType;
typedef struct {
    char value[33];
} GuestExtKey;
typedef struct {
    GuestDataType type;
    const GuestExtKey *keys;
    uint64_t count;
} GuestExtFilter;
typedef struct {
    GuestExtKey key;
    const void *data;
    uint64_t size;
} GuestExtData;
typedef struct {
    char name[16], term;
    unsigned char reserved[3];
} GuestOnlineId;
typedef struct {
    GuestOnlineId online;
    int32_t platform;
} GuestPeer;
typedef struct {
    uint64_t account;
    int32_t platform;
    unsigned char reserved[4];
} GuestPeerA;
typedef void(PSN_GUEST_ABI *BasicCallback)(int32_t, int32_t, const GuestPeer *, const GuestPeer *,
                                           const GuestDataType *, const void *, uint64_t, void *);
typedef void(PSN_GUEST_ABI *ServiceCallback)(int32_t, int32_t, const char *, uint32_t,
                                             const GuestPeer *, const GuestPeer *,
                                             const GuestDataType *, const void *, uint64_t, void *);
typedef void(PSN_GUEST_ABI *ExtendedCallback)(int32_t, int32_t, const char *, uint32_t,
                                              const GuestPeerA *, const GuestOnlineId *,
                                              const GuestPeerA *, const GuestOnlineId *,
                                              const GuestDataType *, const void *, uint64_t,
                                              const GuestExtData *, uint64_t, void *);
_Static_assert(sizeof(GuestContent) == 32, "PS4 content layout");
_Static_assert(sizeof(GuestResponse) == 32, "PS4 response layout");
_Static_assert(sizeof(GuestOnlineId) == 20 && sizeof(GuestPeerA) == 16, "PS4 peer layouts");
_Static_assert(offsetof(GuestExtFilter, keys) == 72 && sizeof(GuestExtData) == 56,
               "PS4 extended push layouts");

typedef struct Library {
    struct Library *next;
    int32_t id;
    uint64_t pool_size;
} Library;
typedef struct Context {
    struct Context *next;
    int32_t id, library, user;
} Context;
typedef struct Handle {
    struct Handle *next;
    int32_t id, library;
} Handle;
typedef struct Request {
    struct Request *next;
    int64_t id;
    int32_t context, user, method, result, status;
    int dead, sending, completed, aborted, has_response;
    uint32_t timeout_ms;
    uint64_t expected_size, backend, body_size, read_offset;
    int has_content;
    char *group, *path, *type;
    PsnHeader headers[PSN_MAX_HEADERS], response_headers[PSN_MAX_HEADERS];
    uint32_t header_count, response_header_count;
    void *body, *error;
    uint64_t error_size;
} Request;
typedef struct FilterParam {
    char type[65];
    char keys[PSN_MAX_EXT_DATA][33];
    uint32_t key_count;
} FilterParam;
typedef struct Filter {
    struct Filter *next;
    int32_t id, library, handle;
    uint32_t kind, label, count, references;
    char *service;
    FilterParam params[MAX_FILTER_PARAMS];
} Filter;
typedef struct Registration {
    struct Registration *next;
    int32_t id, context;
    void *callback, *arg;
    Filter *filter;
    uint64_t cookies[MAX_FILTER_PARAMS];
    unsigned in_flight;
    int retired, waiting;
} Registration;
typedef struct Push {
    struct Push *next;
    PsnPushEvent event;
    PsnExtData ext[PSN_MAX_EXT_DATA];
    char service[256], type[65], keys[PSN_MAX_EXT_DATA][33];
    uint64_t allocation_size;
} Push;

static pthread_mutex_t web_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_idle = PTHREAD_COND_INITIALIZER;
static __thread Registration *active_callback;
static Library *libraries;
static Context *contexts;
static Handle *handles;
static Request *requests;
static Filter *filters;
static Registration *registrations;
static Push *push_head, *push_tail;
static unsigned queue_count;
static uint64_t queue_bytes, next_cookie = 1;
static int32_t next_id = 1;
static int dispatching;

/* Handles never alias an earlier handle during the process lifetime. */
static int32_t allocate_id(void) { return next_id < INT32_MAX ? next_id++ : WEB_NOMEM; }
static Library *find_library(int32_t id) {
    for (Library *p = libraries; p; p = p->next)
        if (p->id == id)
            return p;
    return NULL;
}
static Context *find_context(int32_t id) {
    for (Context *p = contexts; p; p = p->next)
        if (p->id == id)
            return p;
    return NULL;
}
static Handle *find_handle(int32_t lib, int32_t id) {
    for (Handle *p = handles; p; p = p->next)
        if (p->id == id && p->library == lib)
            return p;
    return NULL;
}
static Request *find_request(int64_t id) {
    for (Request *p = requests; p; p = p->next)
        if (p->id == id && !p->dead)
            return p;
    return NULL;
}
static Filter *find_filter(int32_t id) {
    for (Filter *p = filters; p; p = p->next)
        if (p->id == id)
            return p;
    return NULL;
}
static char *copy_string(const char *value, size_t maximum) {
    if (!value)
        return NULL;
    size_t size = strnlen(value, maximum + 1);
    if (size > maximum)
        return NULL;
    char *copy = malloc(size + 1);
    if (copy)
        memcpy(copy, value, size + 1);
    return copy;
}
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int32_t translate_error(int32_t error) {
    if (error == PSN_INVALID_ARGUMENT)
        return WEB_INVALID;
    if (error == PSN_BACKEND_NO_MEMORY)
        return WEB_NOMEM;
    if (error == PSN_BACKEND_ABORTED)
        return WEB_ABORTED;
    return error;
}
static void free_headers(PsnHeader *headers, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        free((void *)headers[i].name);
        free((void *)headers[i].value);
    }
}
static void release_backend(Request *request, int cancel) {
    if (!request->backend)
        return;
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    if (backend) {
        if (cancel)
            backend->web_cancel(instance, request->backend);
        backend->web_release(instance, request->backend);
    }
    psn_host_unlock();
    request->backend = 0;
}
static void free_request(Request *request) {
    Request **link = &requests;
    while (*link && *link != request)
        link = &(*link)->next;
    if (*link)
        *link = request->next;
    free(request->group);
    free(request->path);
    free(request->type);
    free_headers(request->headers, request->header_count);
    free_headers(request->response_headers, request->response_header_count);
    free(request->body);
    free(request->error);
    free(request);
}
static void retire_request(Request *request) {
    request->dead = 1;
    request->aborted = 1;
    release_backend(request, 1);
    /* A synchronous SendRequest owns the allocation until its polling loop returns. */
    if (!request->sending)
        free_request(request);
}
static void free_push(Push *push) {
    free((void *)push->event.data.data);
    for (uint32_t i = 0; i < push->event.ext_data_count; i++)
        free((void *)push->ext[i].value.data);
    free(push);
}
static void remove_registration(Registration *registration) {
    Registration **link = &registrations;
    while (*link && *link != registration)
        link = &(*link)->next;
    if (*link)
        *link = registration->next;
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    if (backend)
        for (uint32_t i = 0; i < registration->filter->count; i++)
            if (registration->cookies[i])
                backend->unsubscribe(instance, registration->cookies[i]);
    psn_host_unlock();
    registration->filter->references--;
    registration->retired = 1;
    /* A callback may remove itself. Another thread waits for the pinned callback
     * frame before its user argument can be safely released by the caller. */
    if (registration->in_flight && active_callback == registration)
        return;
    registration->waiting = 1;
    while (registration->in_flight)
        pthread_cond_wait(&callback_idle, &web_lock);
    free(registration);
}
static void remove_context(Context *context) {
    Context **link = &contexts;
    while (*link && *link != context)
        link = &(*link)->next;
    if (*link)
        *link = context->next;
    Request *req = requests;
    while (req) {
        Request *next = req->next;
        if (req->context == context->id)
            retire_request(req);
        req = next;
    }
    /* Remove the public handle before waiting for a callback. A callback can then
     * delete this context again without finding an object being retired. */
    for (;;) {
        Registration *cb = registrations;
        while (cb && cb->context != context->id)
            cb = cb->next;
        if (!cb)
            break;
        remove_registration(cb);
    }
    free(context);
}

static PSN_GUEST_ABI int32_t web_initialize(int32_t http, uint64_t pool_size) {
    (void)http;
    Library *library = calloc(1, sizeof(*library));
    if (!library)
        return WEB_NOMEM;
    pthread_mutex_lock(&web_lock);
    unsigned count = 0;
    for (Library *p = libraries; p; p = p->next)
        count++;
    if (count >= 32) {
        pthread_mutex_unlock(&web_lock);
        free(library);
        return WEB_NOMEM;
    }
    library->id = allocate_id();
    library->pool_size = pool_size;
    library->next = libraries;
    libraries = library;
    int32_t id = library->id;
    pthread_mutex_unlock(&web_lock);
    return id;
}
static PSN_GUEST_ABI int32_t web_create_context_a(int32_t library, int32_t user) {
    if (!psn_host_has_local_user(user))
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    if (!find_library(library)) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NO_LIB;
    }
    unsigned count = 0;
    for (Context *p = contexts; p; p = p->next) {
        count++;
        if (p->library == library && p->user == user) {
            pthread_mutex_unlock(&web_lock);
            return WEB_ERROR(10);
        }
    }
    if (count >= MAX_OBJECTS) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NOMEM;
    }
    Context *context = calloc(1, sizeof(*context));
    if (!context) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NOMEM;
    }
    context->id = allocate_id();
    context->library = library;
    context->user = user;
    context->next = contexts;
    contexts = context;
    int32_t id = context->id;
    pthread_mutex_unlock(&web_lock);
    return id;
}
static PSN_GUEST_ABI int32_t web_create_context(int32_t library, const GuestOnlineId *online) {
    if (!online || strnlen(online->name, 17) > 16)
        return WEB_INVALID;
    int32_t user = -1, users[PSN_MAX_LOCAL_USERS];
    uint32_t count = psn_host_local_users(users);
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    if (backend)
        for (uint32_t i = 0; i < count; i++) {
            PsnIdentity identity = {0};
            if (!backend->identity(instance, users[i], &identity) && identity.state == 2 &&
                !strncmp(identity.online_id, online->name, 17)) {
                user = users[i];
                break;
            }
        }
    psn_host_unlock();
    return user >= 0 ? web_create_context_a(library, user) : WEB_ERROR(16);
}
static PSN_GUEST_ABI int32_t web_delete_context(int32_t id) {
    pthread_mutex_lock(&web_lock);
    Context *context = find_context(id);
    if (context)
        remove_context(context);
    pthread_mutex_unlock(&web_lock);
    return context ? 0 : WEB_NO_CONTEXT;
}
static PSN_GUEST_ABI int32_t web_create_request(int32_t context, const char *group,
                                                const char *path, int32_t method,
                                                const GuestContent *content, int64_t *output) {
    if (!group || !path || !output || method < 0 || method > 4 ||
        (content && (content->length > PSN_MAX_BODY || (content->length && !content->type))))
        return WEB_INVALID;
    if (strnlen(group, 257) > 256 || strnlen(path, 8193) > 8192 ||
        (content && content->type && strnlen(content->type, 1025) > 1024))
        return WEB_ERROR(24);
    Request *request = calloc(1, sizeof(*request));
    if (!request)
        return WEB_NOMEM;
    request->group = copy_string(group, 256);
    request->path = copy_string(path, 8192);
    request->type = copy_string(content && content->type ? content->type : "", 1024);
    if (!request->group || !request->path || !request->type) {
        free(request->group);
        free(request->path);
        free(request->type);
        free(request);
        return WEB_NOMEM;
    }
    pthread_mutex_lock(&web_lock);
    Context *owner = find_context(context);
    unsigned count = 0;
    for (Request *p = requests; p; p = p->next)
        count++;
    if (!owner || count >= MAX_OBJECTS) {
        free_request(request);
        pthread_mutex_unlock(&web_lock);
        return owner ? WEB_NOMEM : WEB_NO_CONTEXT;
    }
    request->id = allocate_id();
    request->context = context;
    request->user = owner->user;
    request->method = method;
    request->expected_size = content ? content->length : 0;
    request->has_content = content != NULL;
    request->timeout_ms = 30000;
    request->next = requests;
    requests = request;
    *output = request->id;
    pthread_mutex_unlock(&web_lock);
    return 0;
}
static PSN_GUEST_ABI int32_t web_add_header(int64_t id, const char *name, const char *value) {
    if (!name || !value || !*name || strnlen(name, 257) > 256 || strnlen(value, 8193) > 8192 ||
        strchr(name, '\r') || strchr(name, '\n') || strchr(value, '\r') || strchr(value, '\n'))
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    int32_t result = 0;
    if (!request)
        result = WEB_NO_REQUEST;
    else if (request->sending || request->completed || request->aborted)
        result = WEB_AFTER_SEND;
    else if (request->header_count == PSN_MAX_HEADERS)
        result = WEB_NOMEM;
    else {
        char *n = copy_string(name, 256), *v = copy_string(value, 8192);
        if (!n || !v) {
            free(n);
            free(v);
            result = WEB_NOMEM;
        } else
            request->headers[request->header_count++] = (PsnHeader){n, v};
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static int32_t copy_response(Request *request, const PsnWebResponse *response) {
    if (response->body.size > PSN_MAX_BODY || response->error_object.size > PSN_MAX_BODY ||
        (response->body.size && !response->body.data) ||
        (response->error_object.size && !response->error_object.data) ||
        response->header_count > PSN_MAX_HEADERS || (response->header_count && !response->headers))
        return WEB_INVALID;
    request->status = response->http_status;
    request->result = translate_error(response->result);
    if (response->body.size) {
        request->body = malloc((size_t)response->body.size);
        if (!request->body)
            return WEB_NOMEM;
        memcpy(request->body, response->body.data, (size_t)response->body.size);
        request->body_size = response->body.size;
    }
    if (response->error_object.size) {
        request->error = malloc((size_t)response->error_object.size);
        if (!request->error)
            return WEB_NOMEM;
        memcpy(request->error, response->error_object.data, (size_t)response->error_object.size);
        request->error_size = response->error_object.size;
    }
    for (uint32_t i = 0; i < response->header_count; i++) {
        char *name = copy_string(response->headers[i].name, 256),
             *value = copy_string(response->headers[i].value, 8192);
        if (!name || !value) {
            free(name);
            free(value);
            return WEB_NOMEM;
        }
        request->response_headers[request->response_header_count++] = (PsnHeader){name, value};
    }
    request->has_response = 1;
    return 0;
}
static PSN_GUEST_ABI int32_t web_send2(int64_t id, const void *body, uint64_t size,
                                       GuestResponse *response) {
    if (size > PSN_MAX_BODY || (size && !body))
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    if (!request) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NO_REQUEST;
    }
    if (request->aborted || request->sending || request->completed) {
        int32_t e = request->aborted ? WEB_ABORTED : WEB_AFTER_SEND;
        pthread_mutex_unlock(&web_lock);
        return e;
    }
    if (request->has_content && request->expected_size != size) {
        pthread_mutex_unlock(&web_lock);
        return WEB_INVALID;
    }
    void *owned_body = size ? malloc((size_t)size) : NULL;
    if (size && !owned_body) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NOMEM;
    }
    if (size)
        memcpy(owned_body, body, (size_t)size);
    PsnWebRequest input = {.user_id = request->user,
                           .method = (uint32_t)request->method,
                           .api_group = request->group,
                           .path = request->path,
                           .content_type = request->type,
                           .headers = request->headers,
                           .header_count = request->header_count,
                           .body = {owned_body, size}};
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    int32_t result =
        backend ? backend->web_start(instance, &input, &request->backend) : PSN_BACKEND_UNAVAILABLE;
    psn_host_unlock();
    free(owned_body);
    if (result < 0 || !request->backend) {
        release_backend(request, 1);
        pthread_mutex_unlock(&web_lock);
        return result < 0 ? translate_error(result) : WEB_INVALID;
    }
    request->sending = 1;
    uint64_t deadline = now_ms() + request->timeout_ms;
    for (;;) {
        if (request->dead || request->aborted) {
            result = WEB_ABORTED;
            break;
        }
        PsnWebResponse output = {0};
        backend = psn_host_lock(&instance);
        int32_t poll = backend ? backend->web_poll(instance, request->backend, &output)
                               : PSN_BACKEND_UNAVAILABLE;
        if (poll == 0) {
            result = copy_response(request, &output);
            if (!result)
                result = request->result;
        } else if (poll != 1)
            result = poll < 0 ? translate_error(poll) : WEB_INVALID;
        psn_host_unlock();
        if (poll != 1) {
            release_backend(request, 0);
            request->completed = 1;
            break;
        }
        if (now_ms() >= deadline) {
            release_backend(request, 1);
            result = WEB_TIMEOUT;
            request->completed = 1;
            break;
        }
        pthread_mutex_unlock(&web_lock);
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
        pthread_mutex_lock(&web_lock);
    }
    request->result = result;
    request->sending = 0;
    if (response && !request->dead && request->has_response) {
        response->status = request->status;
        response->response_size = request->body_size;
        if (response->error && response->error_size) {
            uint64_t n = request->error_size;
            if (n >= response->error_size)
                n = response->error_size - 1;
            if (n)
                memcpy(response->error, request->error, (size_t)n);
            response->error[n] = 0;
        }
    }
    if (request->dead)
        free_request(request);
    pthread_mutex_unlock(&web_lock);
    return result;
}
static PSN_GUEST_ABI int32_t web_send(int64_t id, const void *body, uint64_t size) {
    return web_send2(id, body, size, NULL);
}
static PSN_GUEST_ABI int32_t web_abort(int64_t id) {
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    if (request) {
        request->aborted = 1;
        release_backend(request, 1);
    }
    pthread_mutex_unlock(&web_lock);
    return request ? 0 : WEB_NO_REQUEST;
}
static PSN_GUEST_ABI int32_t web_delete_request(int64_t id) {
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    if (request)
        retire_request(request);
    pthread_mutex_unlock(&web_lock);
    return request ? 0 : WEB_NO_REQUEST;
}
static PSN_GUEST_ABI int32_t web_timeout(int64_t id, uint32_t ms) {
    if (!ms)
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    int32_t result = 0;
    if (!request)
        result = WEB_NO_REQUEST;
    else if (request->sending || request->completed)
        result = WEB_AFTER_SEND;
    else
        request->timeout_ms = ms;
    pthread_mutex_unlock(&web_lock);
    return result;
}
static PSN_GUEST_ABI int32_t web_read(int64_t id, void *data, uint64_t size) {
    if (!data || !size)
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    int32_t result;
    if (!request)
        result = WEB_NO_REQUEST;
    else if (request->aborted)
        result = WEB_ABORTED;
    else if (!request->completed)
        result = WEB_BUSY;
    else if (!request->has_response)
        result = request->result;
    else {
        uint64_t n = request->body_size - request->read_offset;
        if (n > size)
            n = size;
        if (n > INT32_MAX)
            n = INT32_MAX;
        if (n)
            memcpy(data, (char *)request->body + request->read_offset, (size_t)n);
        request->read_offset += n;
        result = (int32_t)n;
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static PSN_GUEST_ABI int32_t web_status(int64_t id, int32_t *status) {
    if (!status)
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    int32_t result = !request                 ? WEB_NO_REQUEST
                     : request->aborted       ? WEB_ABORTED
                     : !request->completed    ? WEB_BUSY
                     : !request->has_response ? request->result
                                              : 0;
    if (!result)
        *status = request->status;
    pthread_mutex_unlock(&web_lock);
    return result;
}
static int32_t web_header(int64_t id, const char *name, char *value, uint64_t capacity,
                          uint64_t *length) {
    if (!name || (!length && (!value || !capacity)))
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Request *request = find_request(id);
    int32_t result = 0;
    const char *found = NULL;
    if (!request)
        result = WEB_NO_REQUEST;
    else if (request->aborted)
        result = WEB_ABORTED;
    else if (!request->completed)
        result = WEB_BUSY;
    else if (!request->has_response)
        result = request->result;
    else {
        for (uint32_t i = 0; i < request->response_header_count; i++)
            if (!strcasecmp(name, request->response_headers[i].name)) {
                found = request->response_headers[i].value;
                break;
            }
        if (!found)
            result = WEB_INVALID;
        else {
            size_t n = strlen(found);
            if (length)
                *length = n;
            if (value) {
                if (capacity <= n)
                    result = WEB_INVALID;
                else
                    memcpy(value, found, n + 1);
            }
        }
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static PSN_GUEST_ABI int32_t web_header_value(int64_t id, const char *name, char *value,
                                              uint64_t capacity) {
    return web_header(id, name, value, capacity, NULL);
}
static PSN_GUEST_ABI int32_t web_header_length(int64_t id, const char *name, uint64_t *length) {
    if (!length)
        return WEB_INVALID;
    return web_header(id, name, NULL, 0, length);
}

static PSN_GUEST_ABI int32_t web_create_handle(int32_t lib) {
    pthread_mutex_lock(&web_lock);
    if (!find_library(lib)) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NO_LIB;
    }
    unsigned count = 0;
    for (Handle *h = handles; h; h = h->next)
        count++;
    Handle *handle = count < MAX_OBJECTS ? calloc(1, sizeof(*handle)) : NULL;
    if (!handle) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NOMEM;
    }
    handle->id = allocate_id();
    handle->library = lib;
    handle->next = handles;
    handles = handle;
    int32_t result = handle->id;
    pthread_mutex_unlock(&web_lock);
    return result;
}
static PSN_GUEST_ABI int32_t web_delete_handle(int32_t lib, int32_t id) {
    pthread_mutex_lock(&web_lock);
    Handle *handle = find_handle(lib, id);
    int32_t result = 0;
    if (!handle)
        result = WEB_NO_HANDLE;
    for (Filter *filter = filters; !result && filter; filter = filter->next)
        if (filter->library == lib && filter->handle == id)
            result = WEB_ERROR(25);
    if (!result) {
        Handle **link = &handles;
        while (*link != handle)
            link = &(*link)->next;
        *link = handle->next;
        free(handle);
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static int32_t create_filter(int32_t lib, int32_t handle, const char *service, uint32_t label,
                             const void *parameters, uint64_t count, uint32_t kind) {
    if (!parameters || !count || count > MAX_FILTER_PARAMS ||
        (service && strnlen(service, 256) > 255))
        return WEB_INVALID;
    Filter *filter = calloc(1, sizeof(*filter));
    if (!filter)
        return WEB_NOMEM;
    if (service) {
        filter->service = copy_string(service, 255);
        if (!filter->service) {
            free(filter);
            return WEB_NOMEM;
        }
    }
    filter->library = lib;
    filter->handle = handle;
    filter->label = service ? label : UINT32_MAX;
    filter->kind = kind;
    filter->count = (uint32_t)count;
    for (uint32_t i = 0; i < count; i++) {
        const GuestExtFilter *ext = kind == 3 ? &((const GuestExtFilter *)parameters)[i] : NULL;
        const GuestDataType *type = ext ? &ext->type : &((const GuestDataType *)parameters)[i];
        if (strnlen(type->value, 65) > 64 ||
            (ext && (ext->count > PSN_MAX_EXT_DATA || (ext->count && !ext->keys)))) {
            free(filter->service);
            free(filter);
            return WEB_INVALID;
        }
        memcpy(filter->params[i].type, type->value, strlen(type->value) + 1);
        if (ext) {
            filter->params[i].key_count = (uint32_t)ext->count;
            for (uint32_t k = 0; k < ext->count; k++) {
                if (strnlen(ext->keys[k].value, 33) > 32) {
                    free(filter->service);
                    free(filter);
                    return WEB_INVALID;
                }
                memcpy(filter->params[i].keys[k], ext->keys[k].value,
                       strlen(ext->keys[k].value) + 1);
            }
        }
    }
    /* Filter parameters are an OR-list. Duplicate data types must not produce
     * duplicate subscriptions and callback delivery for the same registration. */
    for (uint32_t i = 0; i < filter->count; i++)
        for (uint32_t j = i + 1; j < filter->count;) {
            if (strcmp(filter->params[i].type, filter->params[j].type)) {
                j++;
                continue;
            }
            FilterParam *target = &filter->params[i], *source = &filter->params[j];
            for (uint32_t k = 0; k < source->key_count; k++) {
                uint32_t n = 0;
                while (n < target->key_count && strcmp(target->keys[n], source->keys[k]))
                    n++;
                if (n == target->key_count) {
                    if (n == PSN_MAX_EXT_DATA) {
                        free(filter->service);
                        free(filter);
                        return WEB_INVALID;
                    }
                    memcpy(target->keys[target->key_count++], source->keys[k], 33);
                }
            }
            memmove(&filter->params[j], &filter->params[j + 1],
                    (filter->count - j - 1) * sizeof(filter->params[0]));
            filter->count--;
        }
    pthread_mutex_lock(&web_lock);
    int32_t result = 0;
    unsigned existing = 0;
    for (Filter *f = filters; f; f = f->next)
        existing++;
    if (!find_library(lib))
        result = WEB_NO_LIB;
    else if (kind != 1 && !find_handle(lib, handle))
        result = WEB_NO_HANDLE;
    else if (existing >= MAX_OBJECTS)
        result = WEB_NOMEM;
    if (result) {
        free(filter->service);
        free(filter);
    } else {
        filter->id = allocate_id();
        filter->next = filters;
        filters = filter;
        result = filter->id;
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static PSN_GUEST_ABI int32_t web_create_basic_filter(int32_t lib, const GuestDataType *params,
                                                     uint64_t count) {
    return create_filter(lib, 0, NULL, UINT32_MAX, params, count, 1);
}
static PSN_GUEST_ABI int32_t web_create_service_filter(int32_t lib, int32_t handle,
                                                       const char *service, uint32_t label,
                                                       const GuestDataType *params,
                                                       uint64_t count) {
    if (!service || label == UINT32_MAX)
        return WEB_INVALID;
    return create_filter(lib, handle, service, label, params, count, 2);
}
static PSN_GUEST_ABI int32_t web_create_ext_filter(int32_t lib, int32_t handle, const char *service,
                                                   uint32_t label, const GuestExtFilter *params,
                                                   uint64_t count) {
    if (service && label == UINT32_MAX)
        return WEB_INVALID;
    return create_filter(lib, handle, service, label, params, count, 3);
}
static PSN_GUEST_ABI int32_t web_delete_filter(int32_t lib, int32_t id) {
    pthread_mutex_lock(&web_lock);
    Filter *filter = find_filter(id);
    int32_t result = 0;
    if (!filter || filter->library != lib)
        result = WEB_NO_FILTER;
    else if (filter->references)
        result = WEB_BUSY;
    else {
        Filter **link = &filters;
        while (*link != filter)
            link = &(*link)->next;
        *link = filter->next;
        free(filter->service);
        free(filter);
    }
    pthread_mutex_unlock(&web_lock);
    return result;
}
static int32_t register_callback(int32_t context, int32_t filter_id, void *callback, void *arg,
                                 uint32_t kind) {
    if (!callback)
        return WEB_INVALID;
    pthread_mutex_lock(&web_lock);
    Context *owner = find_context(context);
    Filter *filter = find_filter(filter_id);
    int32_t result = 0;
    unsigned count = 0;
    for (Registration *r = registrations; r; r = r->next)
        count++;
    if (!owner)
        result = WEB_NO_CONTEXT;
    else if (!filter || filter->library != owner->library || filter->kind != kind)
        result = WEB_NO_FILTER;
    else if (count >= MAX_OBJECTS)
        result = WEB_NOMEM;
    Registration *registration = result ? NULL : calloc(1, sizeof(*registration));
    if (!result && !registration)
        result = WEB_NOMEM;
    if (!result) {
        registration->id = allocate_id();
        registration->context = context;
        registration->callback = callback;
        registration->arg = arg;
        registration->filter = filter;
        void *instance;
        const PsnBackendV1 *backend = psn_host_lock(&instance);
        if (!backend)
            result = PSN_BACKEND_UNAVAILABLE;
        for (uint32_t i = 0; !result && i < filter->count; i++) {
            const char *keys[PSN_MAX_EXT_DATA];
            for (uint32_t k = 0; k < filter->params[i].key_count; k++)
                keys[k] = filter->params[i].keys[k];
            PsnPushFilter input = {.user_id = owner->user,
                                   .kind = kind,
                                   .service = filter->service,
                                   .data_type = filter->params[i].type,
                                   .ext_keys = keys,
                                   .ext_key_count = filter->params[i].key_count,
                                   .service_label = filter->label};
            if (next_cookie == UINT64_MAX) {
                result = WEB_NOMEM;
                break;
            }
            uint64_t cookie = next_cookie++;
            result = backend->subscribe(instance, cookie, &input);
            if (!result)
                registration->cookies[i] = cookie;
        }
        if (result && backend)
            for (uint32_t i = 0; i < filter->count; i++)
                if (registration->cookies[i])
                    backend->unsubscribe(instance, registration->cookies[i]);
        psn_host_unlock();
        if (!result) {
            filter->references++;
            registration->next = registrations;
            registrations = registration;
            result = registration->id;
            registration = NULL;
        }
    }
    free(registration);
    pthread_mutex_unlock(&web_lock);
    return translate_error(result);
}
static PSN_GUEST_ABI int32_t web_register_basic(int32_t context, int32_t filter, void *callback,
                                                void *arg) {
    return register_callback(context, filter, callback, arg, 1);
}
static PSN_GUEST_ABI int32_t web_register_service(int32_t context, int32_t filter, void *callback,
                                                  void *arg) {
    return register_callback(context, filter, callback, arg, 2);
}
static PSN_GUEST_ABI int32_t web_register_extended(int32_t context, int32_t filter, void *callback,
                                                   void *arg) {
    return register_callback(context, filter, callback, arg, 3);
}
static PSN_GUEST_ABI int32_t web_unregister(int32_t context, int32_t id) {
    pthread_mutex_lock(&web_lock);
    Registration *found = NULL;
    for (Registration *r = registrations; r; r = r->next)
        if (r->context == context && r->id == id) {
            found = r;
            break;
        }
    if (found)
        remove_registration(found);
    pthread_mutex_unlock(&web_lock);
    return found ? 0 : WEB_NO_CALLBACK;
}

int32_t psn_host_webapi_event(const PsnPushEvent *event) {
    if (!event || !event->subscription || !event->data_type || strnlen(event->data_type, 65) > 64 ||
        (event->service && strnlen(event->service, 256) > 255) ||
        event->data.size > MAX_QUEUE_BYTES || (event->data.size && !event->data.data) ||
        event->ext_data_count > PSN_MAX_EXT_DATA || (event->ext_data_count && !event->ext_data))
        return WEB_INVALID;
    Push *push = calloc(1, sizeof(*push));
    if (!push)
        return WEB_NOMEM;
    push->event = *event;
    push->event.data.data = NULL;
    push->event.ext_data = push->ext;
    push->event.ext_data_count = 0;
    memcpy(push->type, event->data_type, strlen(event->data_type) + 1);
    push->event.data_type = push->type;
    if (event->service) {
        memcpy(push->service, event->service, strlen(event->service) + 1);
        push->event.service = push->service;
    }
    push->allocation_size = sizeof(*push) + event->data.size;
    if (event->data.size) {
        void *data = malloc((size_t)event->data.size);
        if (!data) {
            free_push(push);
            return WEB_NOMEM;
        }
        memcpy(data, event->data.data, (size_t)event->data.size);
        push->event.data.data = data;
    }
    for (uint32_t i = 0; i < event->ext_data_count; i++) {
        const PsnExtData *source = &event->ext_data[i];
        if (!source->key || strnlen(source->key, 33) > 32 || source->value.size > MAX_QUEUE_BYTES ||
            (source->value.size && !source->value.data) ||
            push->allocation_size + source->value.size > MAX_QUEUE_BYTES) {
            free_push(push);
            return WEB_INVALID;
        }
        memcpy(push->keys[i], source->key, strlen(source->key) + 1);
        push->ext[i].key = push->keys[i];
        push->ext[i].value.size = source->value.size;
        if (source->value.size) {
            void *data = malloc((size_t)source->value.size);
            if (!data) {
                free_push(push);
                return WEB_NOMEM;
            }
            memcpy(data, source->value.data, (size_t)source->value.size);
            push->ext[i].value.data = data;
        }
        push->event.ext_data_count++;
        push->allocation_size += source->value.size;
    }
    pthread_mutex_lock(&web_lock);
    if (queue_count >= MAX_OBJECTS || queue_bytes + push->allocation_size > MAX_QUEUE_BYTES) {
        pthread_mutex_unlock(&web_lock);
        free_push(push);
        return WEB_BUSY;
    }
    if (push_tail)
        push_tail->next = push;
    else
        push_head = push;
    push_tail = push;
    queue_count++;
    queue_bytes += push->allocation_size;
    pthread_mutex_unlock(&web_lock);
    return 0;
}
static GuestOnlineId guest_online(const char *name) {
    GuestOnlineId value = {0};
    memcpy(value.name, name, strnlen(name, 16));
    return value;
}
void psn_host_webapi_dispatch(void) {
    pthread_mutex_lock(&web_lock);
    if (dispatching) {
        pthread_mutex_unlock(&web_lock);
        return;
    }
    dispatching = 1;
    /* Detach one finite batch. Reentrant pumps queue work for the next CheckCallback. */
    Push *batch = push_head;
    push_head = push_tail = NULL;
    queue_count = 0;
    queue_bytes = 0;
    while (batch) {
        Push *event = batch;
        batch = batch->next;
        Registration *found = NULL;
        uint32_t parameter = 0;
        for (Registration *r = registrations; !found && r; r = r->next)
            for (uint32_t i = 0; i < r->filter->count; i++)
                if (r->cookies[i] == event->event.subscription) {
                    found = r;
                    parameter = i;
                    break;
                }
        void *callback = NULL, *arg = NULL;
        Registration *pinned = NULL;
        int32_t context = 0, id = 0;
        uint32_t kind = 0, label = 0;
        char service[256] = {0};
        int has_service = 0;
        GuestExtData ext[PSN_MAX_EXT_DATA] = {0};
        uint64_t ext_count = 0;
        if (found) {
            Filter *filter = found->filter;
            FilterParam *param = &filter->params[parameter];
            /* A NULL service means no service, while "" is a named empty service. */
            int same_service = filter->service && event->event.service
                                   ? !strcmp(filter->service, event->event.service)
                                   : !filter->service && !event->event.service;
            if (!strcmp(param->type, event->event.data_type) && same_service &&
                (!filter->service || filter->label == event->event.service_label)) {
                callback = found->callback;
                arg = found->arg;
                context = found->context;
                id = found->id;
                kind = filter->kind;
                label = filter->label;
                pinned = found;
                pinned->in_flight++;
                if (filter->service) {
                    has_service = 1;
                    memcpy(service, filter->service, strlen(filter->service) + 1);
                }
                for (uint32_t i = 0; i < event->event.ext_data_count; i++)
                    for (uint32_t k = 0; k < param->key_count; k++)
                        if (!strcmp(event->ext[i].key, param->keys[k])) {
                            GuestExtData *out = &ext[ext_count++];
                            memcpy(out->key.value, event->ext[i].key,
                                   strlen(event->ext[i].key) + 1);
                            out->data = event->ext[i].value.data;
                            out->size = event->ext[i].value.size;
                            break;
                        }
            }
        }
        /* The dispatch frame owns all data and snapshots the registration, so callbacks
         * may unregister themselves, delete contexts, or call CheckCallback recursively. */
        pthread_mutex_unlock(&web_lock);
        if (callback) {
            active_callback = pinned;
            GuestDataType type = {0};
            memcpy(type.value, event->event.data_type, strlen(event->event.data_type) + 1);
            GuestPeer to = {guest_online(event->event.to_online_id), event->event.to_platform};
            GuestPeer from = {guest_online(event->event.from_online_id),
                              event->event.from_platform};
            if (kind == 1)
                ((BasicCallback)callback)(context, id, &to, &from, &type, event->event.data.data,
                                          event->event.data.size, arg);
            else if (kind == 2)
                ((ServiceCallback)callback)(context, id, has_service ? service : NULL, label, &to,
                                            &from, &type, event->event.data.data,
                                            event->event.data.size, arg);
            else {
                GuestPeerA to_a = {event->event.to_account_id, event->event.to_platform, {0}},
                           from_a = {event->event.from_account_id, event->event.from_platform, {0}};
                ((ExtendedCallback)callback)(
                    context, id, has_service ? service : NULL, label, to_a.account ? &to_a : NULL,
                    to.online.name[0] ? &to.online : NULL, from_a.account ? &from_a : NULL,
                    from.online.name[0] ? &from.online : NULL, &type, event->event.data.data,
                    event->event.data.size, ext_count ? ext : NULL, ext_count, arg);
            }
            active_callback = NULL;
        }
        free_push(event);
        pthread_mutex_lock(&web_lock);
        if (pinned) {
            pinned->in_flight--;
            pthread_cond_broadcast(&callback_idle);
            if (pinned->retired && !pinned->waiting)
                free(pinned);
        }
    }
    dispatching = 0;
    pthread_mutex_unlock(&web_lock);
}
static PSN_GUEST_ABI int32_t web_terminate(int32_t id) {
    pthread_mutex_lock(&web_lock);
    Library *library = find_library(id);
    if (!library) {
        pthread_mutex_unlock(&web_lock);
        return WEB_NO_LIB;
    }
    Library **link = &libraries;
    while (*link != library)
        link = &(*link)->next;
    *link = library->next;
    for (;;) {
        Context *context = contexts;
        while (context && context->library != id)
            context = context->next;
        if (!context)
            break;
        remove_context(context);
    }
    Filter **filter = &filters;
    while (*filter) {
        if ((*filter)->library == id) {
            Filter *old = *filter;
            *filter = old->next;
            free(old->service);
            free(old);
        } else
            filter = &(*filter)->next;
    }
    Handle **handle = &handles;
    while (*handle) {
        if ((*handle)->library == id) {
            Handle *old = *handle;
            *handle = old->next;
            free(old);
        } else
            handle = &(*handle)->next;
    }
    free(library);
    pthread_mutex_unlock(&web_lock);
    return 0;
}
void psn_host_webapi_reset(void) {
    pthread_mutex_lock(&web_lock);
    while (registrations)
        remove_registration(registrations);
    while (contexts)
        remove_context(contexts);
    while (filters) {
        Filter *old = filters;
        filters = old->next;
        free(old->service);
        free(old);
    }
    while (handles) {
        Handle *old = handles;
        handles = old->next;
        free(old);
    }
    while (libraries) {
        Library *old = libraries;
        libraries = old->next;
        free(old);
    }
    while (push_head) {
        Push *old = push_head;
        push_head = old->next;
        free_push(old);
    }
    push_tail = NULL;
    queue_count = 0;
    queue_bytes = 0;
    pthread_mutex_unlock(&web_lock);
}

uintptr_t psn_host_webapi_resolve(const char *name) {
    static const PsnHostExport exports[] = {
        {"sceNpWebApiInitialize", web_initialize},
        {"sceNpWebApiTerminate", web_terminate},
        {"sceNpWebApiCreateContext", web_create_context},
        {"sceNpWebApiCreateContextA", web_create_context_a},
        {"sceNpWebApiDeleteContext", web_delete_context},
        {"sceNpWebApiCreateRequest", web_create_request},
        {"sceNpWebApiSendRequest", web_send},
        {"sceNpWebApiSendRequest2", web_send2},
        {"sceNpWebApiAddHttpRequestHeader", web_add_header},
        {"sceNpWebApiReadData", web_read},
        {"sceNpWebApiAbortRequest", web_abort},
        {"sceNpWebApiDeleteRequest", web_delete_request},
        {"sceNpWebApiSetRequestTimeout", web_timeout},
        {"sceNpWebApiGetHttpStatusCode", web_status},
        {"sceNpWebApiGetHttpResponseHeaderValue", web_header_value},
        {"sceNpWebApiGetHttpResponseHeaderValueLength", web_header_length},
        {"sceNpWebApiCreateHandle", web_create_handle},
        {"sceNpWebApiDeleteHandle", web_delete_handle},
        {"sceNpWebApiCreatePushEventFilter", web_create_basic_filter},
        {"sceNpWebApiCreateServicePushEventFilter", web_create_service_filter},
        {"sceNpWebApiCreateExtdPushEventFilter", web_create_ext_filter},
        {"sceNpWebApiDeletePushEventFilter", web_delete_filter},
        {"sceNpWebApiDeleteServicePushEventFilter", web_delete_filter},
        {"sceNpWebApiDeleteExtdPushEventFilter", web_delete_filter},
        {"sceNpWebApiRegisterPushEventCallback", web_register_basic},
        {"sceNpWebApiRegisterServicePushEventCallback", web_register_service},
        {"sceNpWebApiRegisterExtdPushEventCallbackA", web_register_extended},
        {"sceNpWebApiUnregisterPushEventCallback", web_unregister},
        {"sceNpWebApiUnregisterServicePushEventCallback", web_unregister},
        {"sceNpWebApiUnregisterExtdPushEventCallback", web_unregister},
    };
    return PSN_LOOKUP(exports, name);
}
