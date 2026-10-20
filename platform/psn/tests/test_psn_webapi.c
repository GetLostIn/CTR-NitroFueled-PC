/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Contract tests only: no game data, network, Steam client or renderer. */
#define _GNU_SOURCE
#include "psn_host.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint64_t length;
    const char *type;
    unsigned char reserved[16];
} Content;
typedef struct {
    int32_t status;
    char *error;
    uint64_t error_size, response_size;
} Response;
typedef struct {
    char value[65];
} DataType;
typedef struct {
    char value[33];
} ExtKey;
typedef struct {
    DataType type;
    ExtKey *keys;
    uint64_t count;
} ExtFilter;
typedef struct {
    ExtKey key;
    const void *data;
    uint64_t size;
} ExtData;
typedef struct {
    char name[16], term;
    unsigned char reserved[3];
} OnlineId;
typedef struct {
    OnlineId online;
    int32_t platform;
} Peer;
typedef struct {
    uint64_t account;
    int32_t platform;
    unsigned char reserved[4];
} PeerA;
#define WEB_ERROR(n) ((int32_t)(UINT32_C(0x80552900) | (n)))
#define FN(name, type) ((type)psn_host_webapi_resolve(name))
typedef int32_t(PSN_GUEST_ABI *Initialize)(int32_t, uint64_t);
typedef int32_t(PSN_GUEST_ABI *One)(int32_t);
typedef int32_t(PSN_GUEST_ABI *Two)(int32_t, int32_t);
typedef int32_t(PSN_GUEST_ABI *ReqOne)(int64_t);
typedef int32_t(PSN_GUEST_ABI *CreateRequest)(int32_t, const char *, const char *, int32_t,
                                              const Content *, int64_t *);
typedef int32_t(PSN_GUEST_ABI *Send)(int64_t, const void *, uint64_t, Response *);
typedef int32_t(PSN_GUEST_ABI *AddHeader)(int64_t, const char *, const char *);
typedef int32_t(PSN_GUEST_ABI *Read)(int64_t, void *, uint64_t);
typedef int32_t(PSN_GUEST_ABI *Status)(int64_t, int32_t *);
typedef int32_t(PSN_GUEST_ABI *Header)(int64_t, const char *, char *, uint64_t);
typedef int32_t(PSN_GUEST_ABI *HeaderLength)(int64_t, const char *, uint64_t *);
typedef int32_t(PSN_GUEST_ABI *Timeout)(int64_t, uint32_t);
typedef int32_t(PSN_GUEST_ABI *CreateBasic)(int32_t, const DataType *, uint64_t);
typedef int32_t(PSN_GUEST_ABI *CreateService)(int32_t, int32_t, const char *, uint32_t,
                                              const DataType *, uint64_t);
typedef int32_t(PSN_GUEST_ABI *CreateExtended)(int32_t, int32_t, const char *, uint32_t,
                                               const ExtFilter *, uint64_t);
typedef int32_t(PSN_GUEST_ABI *Register)(int32_t, int32_t, void *, void *);

static int32_t test_user = 1;
uint32_t psn_host_local_users(int32_t users[PSN_MAX_LOCAL_USERS]) {
    users[0] = test_user;
    return 1;
}
int psn_host_has_local_user(int32_t user) { return user == test_user; }

static pthread_mutex_t backend_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int started, pending, canceled, released, subscriptions, unsubscriptions;
static unsigned char reply[] = {1, 0, 3, 4};
static unsigned char request_bytes[] = {7, 0, 9};
static unsigned char *guest_body;
static char saved_path[64], saved_type[128], saved_header[64];
static unsigned char saved_body[8];
static int32_t backend_result, poll_failure;
static PsnHeader response_headers[] = {{"Content-Type", "application/octet-stream"},
                                       {"X-Test", "preserved"}};
static char error_object[] = "{\"error\":17}";
typedef struct {
    uint64_t cookie;
    char type[65], service[256], keys[2][33];
    uint32_t key_count, label;
    int live;
} Subscription;
static Subscription subs[64];
static PsnBackendV1 backend;

const PsnBackendV1 *psn_host_lock(void **instance) {
    pthread_mutex_lock(&backend_lock);
    *instance = &backend;
    return &backend;
}
void psn_host_unlock(void) { pthread_mutex_unlock(&backend_lock); }
static int32_t backend_identity(void *instance, int32_t user, PsnIdentity *identity) {
    assert(instance == &backend);
    memset(identity, 0, sizeof(*identity));
    identity->state = user == test_user ? 2 : 1;
    strcpy(identity->online_id, "Tester");
    return 0;
}
static int32_t backend_start(void *instance, const PsnWebRequest *request, uint64_t *id) {
    assert(instance == &backend && request->user_id == test_user);
    assert(request->body.data != guest_body); /* A native module never receives guest memory. */
    assert(request->body.size <= sizeof(saved_body));
    strcpy(saved_path, request->path);
    strcpy(saved_type, request->content_type);
    if (request->header_count)
        strcpy(saved_header, request->headers[0].value);
    if (request->header_count > 1) {
        assert(request->header_count == 2);
        assert(!strcmp(request->headers[0].name, "X-Custom"));
        assert(!strcmp(request->headers[1].name, "X-Custom") &&
               !strcmp(request->headers[1].value, "second"));
    }
    if (request->body.size)
        memcpy(saved_body, request->body.data, (size_t)request->body.size);
    *id = 99;
    atomic_store(&started, 1);
    return 0;
}
static int32_t backend_poll(void *instance, uint64_t id, PsnWebResponse *response) {
    assert(instance == &backend && id == 99);
    if (atomic_load(&pending))
        return 1;
    if (poll_failure)
        return poll_failure;
    *response = (PsnWebResponse){.result = backend_result,
                                 .http_status = backend_result ? 403 : 201,
                                 .headers = response_headers,
                                 .header_count = 2,
                                 .body = {reply, sizeof(reply)},
                                 .error_object = {error_object, strlen(error_object)}};
    return 0;
}
static void backend_cancel(void *instance, uint64_t id) {
    assert(instance == &backend && id == 99);
    atomic_fetch_add(&canceled, 1);
}
static void backend_release(void *instance, uint64_t id) {
    assert(instance == &backend && id == 99);
    atomic_fetch_add(&released, 1);
    memset(reply, 0xee, sizeof(reply));
}
static int32_t backend_subscribe(void *instance, uint64_t cookie, const PsnPushFilter *filter) {
    assert(instance == &backend && cookie && filter->user_id == test_user);
    unsigned index = (unsigned)atomic_fetch_add(&subscriptions, 1);
    assert(index < 64);
    Subscription *sub = &subs[index];
    sub->cookie = cookie;
    sub->live = 1;
    strcpy(sub->type, filter->data_type);
    strcpy(sub->service, filter->service ? filter->service : "");
    sub->label = filter->service_label;
    assert(filter->ext_key_count <= 2);
    sub->key_count = filter->ext_key_count;
    for (uint32_t i = 0; i < sub->key_count; i++)
        strcpy(sub->keys[i], filter->ext_keys[i]);
    return 0;
}
static void backend_unsubscribe(void *instance, uint64_t cookie) {
    assert(instance == &backend);
    for (int i = 0; i < atomic_load(&subscriptions); i++)
        if (subs[i].cookie == cookie) {
            assert(subs[i].live);
            subs[i].live = 0;
            atomic_fetch_add(&unsubscriptions, 1);
            return;
        }
    assert(0);
}
static void delay_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}
static void wait_flag(atomic_int *flag) {
    for (int i = 0; i < 2000 && !atomic_load(flag); i++)
        delay_ms(1);
    assert(atomic_load(flag));
}
static int32_t new_library(void) {
    int32_t id = FN("sceNpWebApiInitialize", Initialize)(0, 1024 * 1024);
    assert(id > 0);
    return id;
}
static int32_t new_context(int32_t library) {
    int32_t id = FN("sceNpWebApiCreateContextA", Two)(library, test_user);
    assert(id > 0);
    return id;
}
static int64_t new_request(int32_t context) {
    Content content = {.length = sizeof(request_bytes),
                       .type = "multipart/form-data; boundary=exact"};
    int64_t id = 0;
    assert(!FN("sceNpWebApiCreateRequest", CreateRequest)(
        context, "sessionInvitation", "/v1/sessions?fields=all", 1, &content, &id));
    assert(id > 0);
    return id;
}
static void test_requests(void) {
    int32_t lib = new_library(), ctx = new_context(lib);
    int64_t req = new_request(ctx);
    assert(!FN("sceNpWebApiAddHttpRequestHeader", AddHeader)(req, "X-Custom", "retained"));
    assert(!FN("sceNpWebApiAddHttpRequestHeader", AddHeader)(req, "X-Custom", "second"));
    char error[64] = {0};
    Response response = {.error = error, .error_size = sizeof(error)};
    guest_body = request_bytes;
    memcpy(reply, (unsigned char[]){1, 0, 3, 4}, 4);
    assert(
        !FN("sceNpWebApiSendRequest2", Send)(req, request_bytes, sizeof(request_bytes), &response));
    assert(!strcmp(saved_path, "/v1/sessions?fields=all"));
    assert(!strcmp(saved_type, "multipart/form-data; boundary=exact"));
    assert(!strcmp(saved_header, "retained"));
    assert(!memcmp(saved_body, request_bytes, 3));
    assert(response.status == 201 && response.response_size == 4 && !strcmp(error, error_object));
    unsigned char read[8] = {0};
    assert(FN("sceNpWebApiReadData", Read)(req, read, 2) == 2);
    assert(read[0] == 1 && read[1] == 0);
    assert(FN("sceNpWebApiReadData", Read)(req, read + 2, 6) == 2);
    assert(read[2] == 3 && read[3] == 4);
    assert(FN("sceNpWebApiReadData", Read)(req, read, 8) == 0);
    int32_t status = 0;
    assert(!FN("sceNpWebApiGetHttpStatusCode", Status)(req, &status) && status == 201);
    char header[32];
    uint64_t length = 0;
    assert(
        !FN("sceNpWebApiGetHttpResponseHeaderValueLength", HeaderLength)(req, "x-test", &length) &&
        length == 9);
    assert(!FN("sceNpWebApiGetHttpResponseHeaderValue", Header)(req, "X-TEST", header,
                                                                sizeof(header)) &&
           !strcmp(header, "preserved"));
    assert(FN("sceNpWebApiAddHttpRequestHeader", AddHeader)(req, "Late", "no") == WEB_ERROR(30));
    assert(!FN("sceNpWebApiDeleteRequest", ReqOne)(req));
    assert(FN("sceNpWebApiReadData", Read)(req, read, 1) == WEB_ERROR(6));
    req = new_request(ctx);
    backend_result = -171;
    response = (Response){.error = error, .error_size = 5};
    memcpy(reply, (unsigned char[]){5, 0, 7, 8}, 4);
    assert(FN("sceNpWebApiSendRequest2", Send)(req, request_bytes, 3, &response) == -171);
    assert(response.status == 403 && !memcmp(error, "{\"er\0", 5));
    backend_result = 0;
    assert(FN("sceNpWebApiReadData", Read)(req, read, sizeof(read)) == 4 &&
           !memcmp(read, (unsigned char[]){5, 0, 7, 8}, 4));
    assert(!FN("sceNpWebApiGetHttpStatusCode", Status)(req, &status) && status == 403);
    assert(!FN("sceNpWebApiGetHttpResponseHeaderValue", Header)(req, "X-Test", header,
                                                                sizeof(header)) &&
           !strcmp(header, "preserved"));
    assert(!FN("sceNpWebApiDeleteRequest", ReqOne)(req));
    req = new_request(ctx);
    poll_failure = PSN_BACKEND_UNAVAILABLE;
    response = (Response){
        .status = 12345, .error = error, .error_size = sizeof(error), .response_size = 99};
    assert(FN("sceNpWebApiSendRequest2", Send)(req, request_bytes, 3, &response) == poll_failure);
    assert(response.status == 12345 && response.response_size == 99);
    memset(read, 0xaa, sizeof(read));
    status = 12345;
    length = 99;
    strcpy(header, "untouched");
    assert(FN("sceNpWebApiReadData", Read)(req, read, sizeof(read)) == poll_failure &&
           read[0] == 0xaa);
    assert(FN("sceNpWebApiGetHttpStatusCode", Status)(req, &status) == poll_failure &&
           status == 12345);
    assert(FN("sceNpWebApiGetHttpResponseHeaderValue", Header)(req, "X-Test", header,
                                                               sizeof(header)) == poll_failure &&
           !strcmp(header, "untouched"));
    assert(FN("sceNpWebApiGetHttpResponseHeaderValueLength",
              HeaderLength)(req, "X-Test", &length) == poll_failure &&
           length == 99);
    poll_failure = 0;
    assert(!FN("sceNpWebApiDeleteContext", One)(ctx));
    assert(!FN("sceNpWebApiTerminate", One)(lib));
}
typedef struct {
    int64_t id;
    int32_t result;
} SendThread;
static void *send_thread(void *opaque) {
    SendThread *thread = opaque;
    thread->result = FN("sceNpWebApiSendRequest2", Send)(thread->id, request_bytes, 3, NULL);
    return NULL;
}
static void test_abort_and_delete(void) {
    int32_t lib = new_library(), ctx = new_context(lib);
    SendThread send = {new_request(ctx), 0};
    pthread_t thread;
    atomic_store(&pending, 1);
    atomic_store(&started, 0);
    int before = atomic_load(&released);
    assert(!pthread_create(&thread, NULL, send_thread, &send));
    wait_flag(&started);
    assert(!FN("sceNpWebApiAbortRequest", ReqOne)(send.id));
    assert(!pthread_join(thread, NULL));
    assert(send.result == WEB_ERROR(9) && atomic_load(&released) == before + 1);
    assert(!FN("sceNpWebApiDeleteRequest", ReqOne)(send.id));
    send.id = new_request(ctx);
    atomic_store(&started, 0);
    assert(!pthread_create(&thread, NULL, send_thread, &send));
    wait_flag(&started);
    assert(!FN("sceNpWebApiDeleteContext", One)(ctx));
    assert(!pthread_join(thread, NULL));
    assert(send.result == WEB_ERROR(9));
    ctx = new_context(lib);
    send.id = new_request(ctx);
    assert(!FN("sceNpWebApiSetRequestTimeout", Timeout)(send.id, 2));
    assert(FN("sceNpWebApiSendRequest2", Send)(send.id, request_bytes, 3, NULL) == WEB_ERROR(31));
    atomic_store(&pending, 0);
    assert(!FN("sceNpWebApiTerminate", One)(lib));
}

static int callback_count, delete_context_in_callback;
static int32_t callback_lib, callback_filter, callback_handle;
static void PSN_GUEST_ABI extended_callback(int32_t context, int32_t id, const char *service,
                                            uint32_t label, const PeerA *to,
                                            const OnlineId *to_name, const PeerA *from,
                                            const OnlineId *from_name, const DataType *type,
                                            const void *data, uint64_t size, const ExtData *ext,
                                            uint64_t count, void *arg) {
    assert(arg == (void *)0x1234 && service && !*service && label == 7);
    assert(to && to->account == 11 && to->platform == 3 && to_name &&
           !strcmp(to_name->name, "Self"));
    assert(from && from->account == 22 && from_name && !strcmp(from_name->name, "Friend"));
    assert(!strcmp(type->value, "np:service:friendlist:friend") && size == 3 &&
           !memcmp(data, "a\0b", 3));
    assert(count == 1 && !strcmp(ext[0].key.value, "trigger") && ext[0].size == 3 &&
           !memcmp(ext[0].data, "add", 3));
    callback_count++;
    if (delete_context_in_callback) {
        assert(!FN("sceNpWebApiDeleteContext", One)(context));
        assert(!FN("sceNpWebApiDeleteExtdPushEventFilter", Two)(callback_lib, callback_filter));
        assert(!FN("sceNpWebApiDeleteHandle", Two)(callback_lib, callback_handle));
    } else
        assert(!FN("sceNpWebApiUnregisterExtdPushEventCallback", Two)(context, id));
    /* Reentrancy and freeing registration/filter cannot invalidate this frame. */
    psn_host_webapi_dispatch();
    assert(!memcmp(data, "a\0b", 3) && !memcmp(ext[0].data, "add", 3));
}
static PsnPushEvent make_event(uint64_t cookie) {
    PsnPushEvent event = {.subscription = cookie,
                          .from_account_id = 22,
                          .to_account_id = 11,
                          .from_platform = 3,
                          .to_platform = 3,
                          .service = "",
                          .data_type = "np:service:friendlist:friend",
                          .data = {"a\0b", 3},
                          .service_label = 7};
    strcpy(event.from_online_id, "Friend");
    strcpy(event.to_online_id, "Self");
    return event;
}
static void test_extended_push(void) {
    int32_t lib = new_library(), ctx = new_context(lib),
            handle = FN("sceNpWebApiCreateHandle", One)(lib);
    assert(handle > 0);
    ExtKey keys[1] = {{{"trigger"}}};
    ExtFilter params[2] = {{{"np:service:friendlist:friend"}, keys, 1},
                           {{"np:service:presence:onlineStatus"}, NULL, 0}};
    int32_t filter =
        FN("sceNpWebApiCreateExtdPushEventFilter", CreateExtended)(lib, handle, "", 7, params, 2);
    assert(filter > 0);
    int before = atomic_load(&subscriptions);
    int32_t cb = FN("sceNpWebApiRegisterExtdPushEventCallbackA",
                    Register)(ctx, filter, (void *)extended_callback, (void *)0x1234);
    assert(cb > 0);
    assert(atomic_load(&subscriptions) == before + 2 && subs[before].key_count == 1 &&
           !strcmp(subs[before].keys[0], "trigger"));
    char data[] = {'a', 0, 'b'}, extra[] = "add";
    PsnExtData ext[] = {{"trigger", {extra, 3}}, {"internal", {"omit", 4}}};
    PsnPushEvent event = make_event(subs[before].cookie);
    event.data = (PsnBytes){data, 3};
    event.ext_data = ext;
    event.ext_data_count = 2;
    event.service = NULL;
    assert(!psn_host_webapi_event(&event));
    psn_host_webapi_dispatch();
    assert(callback_count == 0);
    event.service = "";
    assert(!psn_host_webapi_event(&event));
    memset(data, 'x', sizeof(data));
    memset(extra, 'x', 3);
    assert(callback_count == 0);
    psn_host_webapi_dispatch();
    assert(callback_count == 1);
    assert(!psn_host_webapi_event(&event));
    psn_host_webapi_dispatch();
    assert(callback_count == 1); /* stale cookie */
    before = atomic_load(&subscriptions);
    cb = FN("sceNpWebApiRegisterExtdPushEventCallbackA",
            Register)(ctx, filter, (void *)extended_callback, (void *)0x1234);
    assert(cb > 0);
    event = make_event(subs[before].cookie);
    ext[0].value = (PsnBytes){"add", 3};
    event.ext_data = ext;
    event.ext_data_count = 2;
    assert(!psn_host_webapi_event(&event));
    assert(!psn_host_webapi_event(&event));
    delete_context_in_callback = 1;
    callback_lib = lib;
    callback_filter = filter;
    callback_handle = handle;
    psn_host_webapi_dispatch();
    assert(callback_count == 2);
    assert(!FN("sceNpWebApiTerminate", One)(lib));
    delete_context_in_callback = 0;
}
static int basic_count, service_count;
static void PSN_GUEST_ABI basic_callback(int32_t context, int32_t id, const Peer *to,
                                         const Peer *from, const DataType *type, const void *data,
                                         uint64_t size, void *arg) {
    (void)context;
    (void)id;
    assert(to->platform == 3 && from->platform == 3 && !strcmp(from->online.name, "Friend"));
    assert(!strcmp(type->value, "basic") && size == 3 && !memcmp(data, "a\0b", 3) &&
           arg == (void *)9);
    basic_count++;
}
static void PSN_GUEST_ABI service_callback(int32_t context, int32_t id, const char *service,
                                           uint32_t label, const Peer *to, const Peer *from,
                                           const DataType *type, const void *data, uint64_t size,
                                           void *arg) {
    assert(!strcmp(service, "service") && label == 2);
    assert(!strcmp(type->value, "basic"));
    service_count++;
    basic_callback(context, id, to, from, type, data, size, arg);
}
static void test_basic_and_service(void) {
    int32_t lib = new_library(), ctx = new_context(lib);
    DataType type = {{"basic"}};
    DataType duplicate_types[] = {{{"basic"}}, {{"basic"}}};
    int32_t filter = FN("sceNpWebApiCreatePushEventFilter", CreateBasic)(lib, duplicate_types, 2);
    assert(filter > 0);
    int before = atomic_load(&subscriptions);
    assert(FN("sceNpWebApiRegisterPushEventCallback", Register)(ctx, filter, (void *)basic_callback,
                                                                (void *)9) > 0);
    assert(FN("sceNpWebApiRegisterPushEventCallback", Register)(ctx, filter, (void *)basic_callback,
                                                                (void *)9) > 0);
    assert(atomic_load(&subscriptions) == before + 2);
    PsnPushEvent event = make_event(subs[before].cookie);
    event.service = NULL;
    event.data_type = "basic";
    assert(!psn_host_webapi_event(&event));
    event.subscription = subs[before + 1].cookie;
    assert(!psn_host_webapi_event(&event));
    event.service = "";
    assert(!psn_host_webapi_event(&event)); /* Named empty service is not basic. */
    psn_host_webapi_dispatch();
    assert(basic_count == 2);
    int32_t handle = FN("sceNpWebApiCreateHandle", One)(lib);
    assert(handle > 0);
    filter = FN("sceNpWebApiCreateServicePushEventFilter", CreateService)(lib, handle, "service", 2,
                                                                          &type, 1);
    assert(filter > 0);
    before = atomic_load(&subscriptions);
    assert(FN("sceNpWebApiRegisterServicePushEventCallback",
              Register)(ctx, filter, (void *)service_callback, (void *)9) > 0);
    event.subscription = subs[before].cookie;
    event.service = "wrong";
    event.service_label = 2;
    assert(!psn_host_webapi_event(&event));
    event.service = "service";
    assert(!psn_host_webapi_event(&event));
    psn_host_webapi_dispatch();
    assert(service_count == 1 && basic_count == 3);
    assert(!FN("sceNpWebApiTerminate", One)(lib));
}
static int unqualified_count;
static void PSN_GUEST_ABI unqualified_callback(int32_t context, int32_t id, const char *service,
                                               uint32_t label, const PeerA *to,
                                               const OnlineId *to_name, const PeerA *from,
                                               const OnlineId *from_name, const DataType *type,
                                               const void *data, uint64_t size, const ExtData *ext,
                                               uint64_t count, void *arg) {
    (void)context;
    (void)id;
    (void)to;
    (void)to_name;
    (void)from;
    (void)from_name;
    (void)data;
    (void)size;
    (void)arg;
    assert(!service && label == UINT32_MAX && !strcmp(type->value, "basic") && !ext && !count);
    unqualified_count++;
}
static void test_unqualified_extended(void) {
    int32_t lib = new_library(), ctx = new_context(lib),
            handle = FN("sceNpWebApiCreateHandle", One)(lib);
    ExtFilter params = {.type = {{"basic"}}};
    int32_t filter = FN("sceNpWebApiCreateExtdPushEventFilter", CreateExtended)(lib, handle, NULL,
                                                                                0, &params, 1);
    assert(filter > 0);
    int before = atomic_load(&subscriptions);
    assert(FN("sceNpWebApiRegisterExtdPushEventCallbackA",
              Register)(ctx, filter, (void *)unqualified_callback, NULL) > 0);
    assert(subs[before].label == UINT32_MAX);
    PsnPushEvent event = make_event(subs[before].cookie);
    event.data_type = "basic";
    event.service = NULL;
    assert(!psn_host_webapi_event(&event));
    event.service = "";
    assert(!psn_host_webapi_event(&event));
    event.service = "other";
    assert(!psn_host_webapi_event(&event));
    psn_host_webapi_dispatch();
    assert(unqualified_count == 1);
    assert(!FN("sceNpWebApiTerminate", One)(lib));
}
static atomic_int callback_entered, callback_continue, unregister_entered, unregister_done;
static int32_t concurrent_ctx, concurrent_cb;
static void PSN_GUEST_ABI blocking_callback(int32_t context, int32_t id, const Peer *to,
                                            const Peer *from, const DataType *type,
                                            const void *data, uint64_t size, void *arg) {
    (void)context;
    (void)id;
    (void)to;
    (void)from;
    (void)type;
    (void)data;
    (void)size;
    (void)arg;
    atomic_store(&callback_entered, 1);
    wait_flag(&callback_continue);
}
static void *dispatch_thread(void *arg) {
    (void)arg;
    psn_host_webapi_dispatch();
    return NULL;
}
static void *unregister_thread(void *arg) {
    (void)arg;
    atomic_store(&unregister_entered, 1);
    assert(!FN("sceNpWebApiUnregisterPushEventCallback", Two)(concurrent_ctx, concurrent_cb));
    atomic_store(&unregister_done, 1);
    return NULL;
}
static void test_concurrent_unregister(void) {
    int32_t lib = new_library();
    concurrent_ctx = new_context(lib);
    DataType type = {{"basic"}};
    int32_t filter = FN("sceNpWebApiCreatePushEventFilter", CreateBasic)(lib, &type, 1);
    int before = atomic_load(&subscriptions);
    concurrent_cb = FN("sceNpWebApiRegisterPushEventCallback",
                       Register)(concurrent_ctx, filter, (void *)blocking_callback, NULL);
    assert(concurrent_cb > 0);
    PsnPushEvent event = make_event(subs[before].cookie);
    event.data_type = "basic";
    event.service = NULL;
    assert(!psn_host_webapi_event(&event));
    pthread_t dispatch, unregister;
    assert(!pthread_create(&dispatch, NULL, dispatch_thread, NULL));
    wait_flag(&callback_entered);
    assert(!pthread_create(&unregister, NULL, unregister_thread, NULL));
    wait_flag(&unregister_entered);
    delay_ms(10);
    assert(!atomic_load(&unregister_done));
    atomic_store(&callback_continue, 1);
    assert(!pthread_join(dispatch, NULL));
    assert(!pthread_join(unregister, NULL));
    assert(atomic_load(&unregister_done));
    assert(!FN("sceNpWebApiTerminate", One)(lib));
}
int main(int argc, char **argv) {
    if (argc > 1)
        test_user = (int32_t)strtol(argv[1], NULL, 10);
    backend = (PsnBackendV1){.size = sizeof(backend),
                             .abi_version = PSN_BACKEND_ABI_V1,
                             .identity = backend_identity,
                             .web_start = backend_start,
                             .web_poll = backend_poll,
                             .web_cancel = backend_cancel,
                             .web_release = backend_release,
                             .subscribe = backend_subscribe,
                             .unsubscribe = backend_unsubscribe};
    /* Legacy online-ID lookup must retain arbitrary local IDs, including zero. */
    int32_t lib = new_library();
    OnlineId online = {0};
    strcpy(online.name, "Tester");
    typedef int32_t(PSN_GUEST_ABI *LegacyContext)(int32_t, const OnlineId *);
    int32_t context = FN("sceNpWebApiCreateContext", LegacyContext)(lib, &online);
    assert(context > 0);
    assert(!FN("sceNpWebApiDeleteContext", One)(context));
    assert(!FN("sceNpWebApiTerminate", One)(lib));
    test_requests();
    test_abort_and_delete();
    test_extended_push();
    test_basic_and_service();
    test_unqualified_extended();
    test_concurrent_unregister();
    psn_host_webapi_reset();
    assert(atomic_load(&subscriptions) == atomic_load(&unsubscriptions));
    puts("PSN WebAPI contracts: request ownership, binary responses, cancellation and push "
         "callbacks passed");
    return 0;
}
