/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional PS4 NP guest adapter. This file owns guest handles and memory; the
 * native backend receives only copied values. The built-in RyuNET path never
 * enters this resolver. Structure layouts are checked against public PS4
 * compatibility references; no Sony service or credential format is assumed. */
#include "psn_host.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NP_USER_NOT_FOUND ((int32_t)0x80550007)
#define NP_CALLBACK_REGISTERED ((int32_t)0x80550008)
#define NP_CALLBACK_MISSING ((int32_t)0x80550009)
#define NP_CALLBACK_MAX ((int32_t)0x8055001D)
#define NP_INVALID_SIZE ((int32_t)0x80550011)
#define NP_ABORTED ((int32_t)0x80550012)
#define NP_REQUEST_MAX ((int32_t)0x80550013)
#define NP_REQUEST_MISSING ((int32_t)0x80550014)
#define NP_INVALID_ID ((int32_t)0x80550015)
#define AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define AUTH_INVALID_SIZE ((int32_t)0x80550302)
#define AUTH_ABORTED ((int32_t)0x80550304)
#define AUTH_REQUEST_MAX ((int32_t)0x80550305)
#define AUTH_REQUEST_MISSING ((int32_t)0x80550306)
#define AUTH_INVALID_ID ((int32_t)0x80550307)
#define DIALOG_ERROR(n) ((int32_t)(0x80B80000u + (n)))

typedef struct {
    char data[16], term, padding[3];
} GuestOnlineId;
typedef struct {
    GuestOnlineId online;
    uint8_t options[8], reserved[8];
} GuestNpId;
typedef struct {
    uint64_t size, affinity;
    int32_t priority;
    uint32_t padding;
} GuestAsync;
typedef struct {
    uint64_t size;
    union {
        const GuestOnlineId *online;
        struct {
            int32_t user;
            uint32_t pad;
        };
    } identity;
    const char *client;
    const char *secret_or_scope;
    const char *scope;
} GuestAuth;
typedef struct {
    uint64_t size;
    int32_t user;
    uint32_t padding;
    uint64_t features;
    uint8_t reserved[32];
} GuestPlus;
_Static_assert(sizeof(GuestOnlineId) == 20, "NP online ID layout");
_Static_assert(sizeof(GuestNpId) == 36, "NP ID layout");
_Static_assert(sizeof(GuestAuth) == 40, "ID token parameter layout");
_Static_assert(offsetof(GuestAuth, client) == 16, "auth client offset");
_Static_assert(sizeof(GuestPlus) == 56, "Plus parameter layout");

static pthread_mutex_t identity_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_idle = PTHREAD_COND_INITIALIZER;

static size_t bounded_length(const char *s, size_t capacity) {
    size_t n = 0;
    if (s)
        while (n < capacity && s[n])
            ++n;
    return n;
}
static int copy_string(char *out, size_t capacity, const char *source) {
    if (!source)
        return 0;
    size_t n = bounded_length(source, capacity);
    if (n == capacity)
        return 0;
    memcpy(out, source, n + 1);
    return 1;
}
static void online_to_guest(GuestOnlineId *out, const char *name) {
    memset(out, 0, sizeof(*out));
    memcpy(out->data, name, bounded_length(name, 16));
}
static int32_t get_identity(int32_t user, PsnIdentity *identity) {
    if (!psn_host_has_local_user(user))
        return NP_USER_NOT_FOUND;
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    int32_t rc = backend ? backend->identity(instance, user, identity) : PSN_BACKEND_UNAVAILABLE;
    psn_host_unlock();
    if (!rc && (!memchr(identity->online_id, 0, sizeof(identity->online_id)) ||
                identity->state < 1 || identity->state > 2))
        return PSN_BACKEND_UNAVAILABLE;
    return rc;
}
static int32_t signed_identity(int32_t user, PsnIdentity *identity) {
    int32_t rc = get_identity(user, identity);
    return rc ? rc : identity->state == 2 ? 0 : PSN_SIGNED_OUT;
}
static int32_t user_from_online(const GuestOnlineId *online) {
    if (!online)
        return PSN_INVALID_ARGUMENT;
    GuestOnlineId copied;
    memcpy(&copied, online, sizeof(copied));
    if (copied.term)
        return PSN_INVALID_ARGUMENT;
    int32_t users[PSN_MAX_LOCAL_USERS];
    uint32_t count = psn_host_local_users(users);
    for (uint32_t i = 0; i < count; ++i) {
        int32_t user = users[i];
        PsnIdentity id = {0};
        if (!signed_identity(user, &id)) {
            GuestOnlineId candidate;
            online_to_guest(&candidate, id.online_id);
            if (!strncmp(candidate.data, copied.data, 16))
                return user;
        }
    }
    return NP_USER_NOT_FOUND;
}
static PSN_GUEST_ABI int32_t np_state(int32_t user, int32_t *state) {
    if (!state)
        return PSN_INVALID_ARGUMENT;
    PsnIdentity identity = {0};
    int32_t rc = get_identity(user, &identity);
    if (!rc)
        *state = identity.state;
    return rc;
}
static PSN_GUEST_ABI int32_t np_online(int32_t user, GuestOnlineId *out) {
    if (!out)
        return PSN_INVALID_ARGUMENT;
    PsnIdentity identity = {0};
    int32_t rc = signed_identity(user, &identity);
    if (!rc)
        online_to_guest(out, identity.online_id);
    return rc;
}
static PSN_GUEST_ABI int32_t np_id(int32_t user, GuestNpId *out) {
    if (!out)
        return PSN_INVALID_ARGUMENT;
    GuestOnlineId online;
    int32_t rc = np_online(user, &online);
    if (!rc) {
        memset(out, 0, sizeof(*out));
        out->online = online;
    }
    return rc;
}
static PSN_GUEST_ABI int32_t np_account(int32_t user, uint64_t *out) {
    if (!out)
        return PSN_INVALID_ARGUMENT;
    PsnIdentity identity = {0};
    int32_t rc = signed_identity(user, &identity);
    if (!rc)
        *out = identity.account_id;
    return rc;
}
static PSN_GUEST_ABI int32_t np_user(uint64_t account, int32_t *out) {
    if (!out || !account)
        return PSN_INVALID_ARGUMENT;
    int32_t users[PSN_MAX_LOCAL_USERS];
    uint32_t count = psn_host_local_users(users);
    for (uint32_t i = 0; i < count; ++i) {
        int32_t user = users[i];
        PsnIdentity identity = {0};
        if (!signed_identity(user, &identity) && identity.account_id == account) {
            *out = user;
            return 0;
        }
    }
    return NP_USER_NOT_FOUND;
}
static PSN_GUEST_ABI int32_t compare_online(const GuestOnlineId *a, const GuestOnlineId *b) {
    if (!a || !b)
        return PSN_INVALID_ARGUMENT;
    return strncmp(a->data, b->data, 16) ? (int32_t)0x80550609 : 0;
}
static PSN_GUEST_ABI int32_t compare_id(const GuestNpId *a, const GuestNpId *b) {
    if (!a || !b)
        return PSN_INVALID_ARGUMENT;
    if (compare_online(&a->online, &b->online) ||
        memcmp(a->options, b->options, sizeof(a->options)) ||
        memcmp(a->reserved, b->reserved, sizeof(a->reserved)))
        return (int32_t)0x80550609;
    return 0;
}

/* Request IDs never reuse a retired value within a process. A delete serializes
 * with polling and joins the provider operation before discarding guest outputs. */
typedef struct {
    int32_t id, auth, async, state, result; /* 0 ready, 1 pending, 2 complete */
    uint64_t backend_request;
    int backend_started, aborted;
    uint32_t kind;
    void *output;
    int32_t *issuer;
    char client[129], secret[257], scope[4097];
    int has_timeout;
    PsnAuthTimeout timeout;
} Request;
static Request requests[48];
static uint32_t request_sequence;
static Request *request_find(int32_t id, int auth) {
    for (size_t i = 0; i < 48; ++i)
        if (requests[i].id == id && requests[i].auth == auth)
            return &requests[i];
    return NULL;
}
static int32_t request_new(int auth, int async, const GuestAsync *param) {
    if (async && !param)
        return auth ? AUTH_INVALID_ARGUMENT : PSN_INVALID_ARGUMENT;
    if (async && param->size != sizeof(*param))
        return auth ? AUTH_INVALID_SIZE : NP_INVALID_SIZE;
    pthread_mutex_lock(&identity_mutex);
    size_t active = 0;
    Request *slot = NULL;
    for (size_t i = 0; i < 48; ++i) {
        if (requests[i].id && requests[i].auth == auth)
            ++active;
        if (!requests[i].id && !slot)
            slot = &requests[i];
    }
    int32_t result = auth ? AUTH_REQUEST_MAX : NP_REQUEST_MAX;
    if (slot && active < (auth ? 16u : 32u) && request_sequence < 0x0fffffffu) {
        memset(slot, 0, sizeof(*slot));
        slot->id = (int32_t)((auth ? 0x10000000u : 0x20000000u) + ++request_sequence);
        slot->auth = auth;
        slot->async = async;
        result = slot->id;
    }
    pthread_mutex_unlock(&identity_mutex);
    return result;
}
static PSN_GUEST_ABI int32_t auth_create(void) { return request_new(1, 0, NULL); }
static PSN_GUEST_ABI int32_t auth_create_async(const GuestAsync *param) {
    return request_new(1, 1, param);
}
static PSN_GUEST_ABI int32_t np_create(void) { return request_new(0, 0, NULL); }
static PSN_GUEST_ABI int32_t np_create_async(const GuestAsync *param) {
    return request_new(0, 1, param);
}

static void request_finish_auth(Request *request) {
    if (request->state != 1)
        return;
    PsnAuthResult response = {0};
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    int32_t rc = backend ? backend->auth_poll(instance, request->backend_request, &response)
                         : PSN_BACKEND_UNAVAILABLE;
    psn_host_unlock();
    if (rc == 1)
        return;
    request->result = rc < 0 ? rc : rc ? PSN_BACKEND_UNAVAILABLE : response.result;
    if (!request->result) {
        size_t capacity = request->kind == 1 ? 4097 : 129;
        size_t n = bounded_length(response.value, capacity);
        if (n == capacity)
            request->result = PSN_BACKEND_UNAVAILABLE;
        else {
            /* Buffers remain host-owned until completion/delete; providers never see them. */
            memset(request->output, 0, capacity + 7);
            memcpy(request->output, response.value, n + 1);
            if (request->issuer)
                *request->issuer = response.issuer_id;
        }
    }
    request->state = 2;
}
static int32_t request_poll(int32_t id, int32_t *result, int auth, int wait, int allow_sync) {
    if (!result)
        return auth ? AUTH_INVALID_ARGUMENT : PSN_INVALID_ARGUMENT;
    for (;;) {
        pthread_mutex_lock(&identity_mutex);
        Request *request = request_find(id, auth);
        int32_t rc = 0;
        if (!request)
            rc = auth ? AUTH_REQUEST_MISSING : NP_REQUEST_MISSING;
        else if (!request->async && !allow_sync)
            rc = auth ? AUTH_INVALID_ID : NP_INVALID_ID;
        /* This binding targets the modern PS4 profile used by CTR (SDK >=2.00).
         * An unused async NPAuth request is pending; it has no operation result.
         * Historical SDK-specific conversions are not part of this profile. */
        else if (!request->state)
            rc = auth && request->async ? 1 : auth ? AUTH_INVALID_ID : NP_INVALID_ID;
        else {
            if (auth)
                request_finish_auth(request);
            if (request->state == 2)
                *result = request->result;
            else
                rc = 1;
        }
        pthread_mutex_unlock(&identity_mutex);
        if (rc != 1 || !wait)
            return rc;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
}
static PSN_GUEST_ABI int32_t auth_poll(int32_t id, int32_t *result) {
    return request_poll(id, result, 1, 0, 0);
}
static PSN_GUEST_ABI int32_t auth_wait(int32_t id, int32_t *result) {
    return request_poll(id, result, 1, 1, 0);
}
static PSN_GUEST_ABI int32_t np_poll(int32_t id, int32_t *result) {
    return request_poll(id, result, 0, 0, 0);
}
static PSN_GUEST_ABI int32_t np_wait(int32_t id, int32_t *result) {
    return request_poll(id, result, 0, 1, 0);
}
static int32_t request_abort(int32_t id, int auth) {
    pthread_mutex_lock(&identity_mutex);
    Request *request = request_find(id, auth);
    int32_t rc = request ? 0 : auth ? AUTH_REQUEST_MISSING : NP_REQUEST_MISSING;
    if (request && request->state != 2) {
        if (auth && request->backend_started) {
            void *instance;
            const PsnBackendV1 *backend = psn_host_lock(&instance);
            if (backend)
                backend->auth_cancel(instance, request->backend_request);
            psn_host_unlock();
        }
        request->state = 2;
        request->aborted = 1;
        request->result = auth ? AUTH_ABORTED : NP_ABORTED;
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static void request_release(Request *request) {
    if (request->auth && request->backend_started) {
        void *instance;
        const PsnBackendV1 *backend = psn_host_lock(&instance);
        if (backend) {
            if (request->state == 1)
                backend->auth_cancel(instance, request->backend_request);
            backend->auth_release(instance, request->backend_request);
        }
        psn_host_unlock();
    }
    memset(request, 0, sizeof(*request));
}
static int32_t request_delete(int32_t id, int auth) {
    pthread_mutex_lock(&identity_mutex);
    Request *request = request_find(id, auth);
    int32_t rc = request ? 0 : auth ? AUTH_REQUEST_MISSING : NP_REQUEST_MISSING;
    if (request)
        request_release(request);
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static PSN_GUEST_ABI int32_t auth_abort(int32_t id) { return request_abort(id, 1); }
static PSN_GUEST_ABI int32_t np_abort(int32_t id) { return request_abort(id, 0); }
static PSN_GUEST_ABI int32_t auth_delete(int32_t id) { return request_delete(id, 1); }
static PSN_GUEST_ABI int32_t np_delete(int32_t id) { return request_delete(id, 0); }

static int32_t auth_start(int32_t id, const GuestAuth *param, void *output, int32_t *issuer,
                          uint32_t kind, int variant) {
    if (!param || !output)
        return AUTH_INVALID_ARGUMENT;
    GuestAuth copied = {0};
    size_t param_size = kind == 1 ? 40 : 32;
    if (param->size != param_size)
        return AUTH_INVALID_SIZE;
    memcpy(&copied, param, param_size);
    int32_t user = variant ? copied.identity.user : user_from_online(copied.identity.online);
    if (user < 0)
        return user;
    if (!psn_host_has_local_user(user))
        return AUTH_INVALID_ARGUMENT;
    char client[129] = {0}, secret[257] = {0}, scope[4097] = {0};
    if (!copy_string(client, sizeof(client), copied.client) ||
        !copy_string(scope, sizeof(scope), kind == 1 ? copied.scope : copied.secret_or_scope) ||
        (kind == 1 && !copy_string(secret, sizeof(secret), copied.secret_or_scope)))
        return AUTH_INVALID_ARGUMENT;
    pthread_mutex_lock(&identity_mutex);
    Request *request = request_find(id, 1);
    int32_t rc = 0, async = 1;
    if (!request)
        rc = AUTH_REQUEST_MISSING;
    else if (request->state) {
        rc = request->aborted ? AUTH_ABORTED : AUTH_INVALID_ARGUMENT;
        request->result = rc;
        /* Reusing a request records the rejected Get result. It does not replace
         * the first operation's buffers or finish/cancel an in-flight worker;
         * that worker may subsequently publish its own completion result. */
    } else {
        async = request->async;
        memcpy(request->client, client, sizeof(client));
        memcpy(request->secret, secret, sizeof(secret));
        memcpy(request->scope, scope, sizeof(scope));
        request->output = output;
        request->issuer = issuer;
        request->kind = kind;
        PsnAuthRequest input = {0};
        input.user_id = user;
        input.kind = kind;
        input.api_variant = (uint32_t)variant;
        input.has_timeout = (uint32_t)request->has_timeout;
        input.timeout = request->timeout;
        input.client_id = request->client;
        input.client_secret = kind == 1 ? request->secret : NULL;
        input.scope = request->scope;
        void *instance;
        const PsnBackendV1 *backend = psn_host_lock(&instance);
        rc = backend ? backend->auth_start(instance, &input, &request->backend_request)
                     : PSN_BACKEND_UNAVAILABLE;
        psn_host_unlock();
        if (!rc) {
            request->backend_started = 1;
            request->state = 1;
        } else {
            request->state = 2;
            request->result = rc;
        }
        if (async)
            rc = 0;
    }
    pthread_mutex_unlock(&identity_mutex);
    if (!rc && !async) {
        int32_t operation_result;
        rc = request_poll(id, &operation_result, 1, 1, 1);
        if (!rc)
            rc = operation_result;
    }
    return rc;
}
static PSN_GUEST_ABI int32_t auth_token(int32_t id, const GuestAuth *p, void *out) {
    return auth_start(id, p, out, NULL, 1, 0);
}
static PSN_GUEST_ABI int32_t auth_token_a(int32_t id, const GuestAuth *p, void *out) {
    return auth_start(id, p, out, NULL, 1, 1);
}
static PSN_GUEST_ABI int32_t auth_token_v3(int32_t id, const GuestAuth *p, void *out) {
    return auth_start(id, p, out, NULL, 1, 3);
}
static PSN_GUEST_ABI int32_t auth_code(int32_t id, const GuestAuth *p, void *out, int32_t *issuer) {
    return auth_start(id, p, out, issuer, 2, 0);
}
static PSN_GUEST_ABI int32_t auth_code_a(int32_t id, const GuestAuth *p, void *out,
                                         int32_t *issuer) {
    return auth_start(id, p, out, issuer, 2, 1);
}
static PSN_GUEST_ABI int32_t auth_code_v3(int32_t id, const GuestAuth *p, void *out,
                                          int32_t *issuer) {
    return auth_start(id, p, out, issuer, 2, 3);
}

static PSN_GUEST_ABI int32_t auth_timeout(int32_t id, int32_t retry, uint32_t resolve,
                                          uint32_t connect, uint32_t send, uint32_t receive) {
    if (retry < 0 || (!retry && !resolve && !connect && !send && !receive))
        return AUTH_INVALID_ARGUMENT;
    pthread_mutex_lock(&identity_mutex);
    Request *request = request_find(id, 1);
    int32_t rc = !request ? AUTH_REQUEST_MISSING : request->state ? PSN_BACKEND_UNSUPPORTED : 0;
    if (!rc) {
        request->has_timeout = 1;
        request->timeout = (PsnAuthTimeout){retry, resolve, connect, send, receive};
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}

typedef struct {
    char country[4];
    int8_t age;
    uint8_t padding[3];
} GuestAgeRestriction;
typedef struct {
    uint64_t size;
    int8_t default_age;
    uint8_t padding[3];
    int32_t count;
    const GuestAgeRestriction *countries;
} GuestContentRestriction;
static PSN_GUEST_ABI int32_t set_title(const char *title, const uint8_t *secret) {
    char copied_title[13];
    uint8_t copied_secret[128];
    if (!title || !secret || !copy_string(copied_title, sizeof(copied_title), title) ||
        !copied_title[0])
        return PSN_INVALID_ARGUMENT;
    memcpy(copied_secret, secret, sizeof(copied_secret));
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    PsnBytes bytes = {copied_secret, sizeof(copied_secret)};
    int32_t rc =
        backend ? backend->set_title(instance, copied_title, bytes) : PSN_BACKEND_UNAVAILABLE;
    psn_host_unlock();
    memset(copied_secret, 0, sizeof(copied_secret));
    return rc;
}
static PSN_GUEST_ABI int32_t set_restriction(const GuestContentRestriction *param) {
    if (!param)
        return PSN_INVALID_ARGUMENT;
    GuestContentRestriction copied = *param;
    if (copied.size != sizeof(copied))
        return NP_INVALID_SIZE;
    if (copied.default_age < 0 || copied.count < 0 || copied.count > 256 ||
        (copied.count && !copied.countries))
        return PSN_INVALID_ARGUMENT;
    PsnAgeRestriction countries[256] = {{0}};
    for (int32_t i = 0; i < copied.count; ++i) {
        GuestAgeRestriction guest = copied.countries[i];
        if (guest.country[2] || guest.age < 0)
            return PSN_INVALID_ARGUMENT;
        memcpy(countries[i].country, guest.country, 2);
        countries[i].age = guest.age;
    }
    PsnContentRestriction restriction = {copied.default_age, (uint32_t)copied.count, countries};
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    int32_t rc = backend ? backend->set_content_restriction(instance, &restriction)
                         : PSN_BACKEND_UNAVAILABLE;
    psn_host_unlock();
    return rc;
}

static int32_t policy_request(int32_t id, int32_t user, int kind, uint64_t features, void *out,
                              int8_t *age) {
    if (!psn_host_has_local_user(user))
        return PSN_INVALID_ARGUMENT;
    pthread_mutex_lock(&identity_mutex);
    Request *request = request_find(id, 0);
    int32_t rc = request ? 0 : NP_REQUEST_MISSING;
    if (request && request->state)
        rc = request->aborted ? NP_ABORTED : NP_INVALID_ID;
    if (!rc) {
        PsnPolicy policy = {0};
        void *instance;
        const PsnBackendV1 *backend = psn_host_lock(&instance);
        rc = backend ? backend->policy(instance, user, features, &policy) : PSN_BACKEND_UNAVAILABLE;
        psn_host_unlock();
        if (!rc && kind == 0)
            rc = policy.availability;
        if (!rc && kind == 1) {
            memset(out, 0, 33);
            *(uint8_t *)out = !!policy.plus_authorized;
        }
        if (!rc && kind == 2) {
            uint8_t *restrictions = out;
            *age = (int8_t)policy.age;
            restrictions[0] = !!policy.content_restricted;
            restrictions[1] = !!policy.chat_restricted;
            restrictions[2] = !!policy.ugc_restricted;
        }
        request->state = 2;
        request->result = rc;
        if (request->async)
            rc = 0;
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static PSN_GUEST_ABI int32_t np_available_a(int32_t id, int32_t user) {
    return policy_request(id, user, 0, 0, NULL, NULL);
}
static PSN_GUEST_ABI int32_t np_available(int32_t id, const GuestOnlineId *online) {
    int32_t user = user_from_online(online);
    return user < 0 ? user : np_available_a(id, user);
}
static PSN_GUEST_ABI int32_t np_parental_a(int32_t id, int32_t user, int8_t *age, void *info) {
    if (!age || !info)
        return PSN_INVALID_ARGUMENT;
    return policy_request(id, user, 2, 0, info, age);
}
static PSN_GUEST_ABI int32_t np_parental(int32_t id, const GuestOnlineId *online, int8_t *age,
                                         void *info) {
    int32_t user = user_from_online(online);
    return user < 0 ? user : np_parental_a(id, user, age, info);
}
static PSN_GUEST_ABI int32_t np_plus(int32_t id, const GuestPlus *param, void *out) {
    if (!param || !out)
        return PSN_INVALID_ARGUMENT;
    if (param->size != sizeof(*param))
        return NP_INVALID_SIZE;
    GuestPlus copied = *param;
    if (copied.features < 1 || copied.features > 3)
        return PSN_INVALID_ARGUMENT;
    for (size_t i = 0; i < sizeof(copied.reserved); ++i)
        if (copied.reserved[i])
            return PSN_INVALID_ARGUMENT;
    return policy_request(id, copied.user, 1, copied.features, out, NULL);
}

/* A registry pin protects each selected delivery. Self-unregister is allowed;
 * another thread waits for the running callback. No callback runs under a lock. */
enum {
    CALLBACK_STATE_LEGACY,
    CALLBACK_STATE_TOOLKIT,
    CALLBACK_STATE_A,
    CALLBACK_PRESENCE_LEGACY,
    CALLBACK_PRESENCE_A,
    CALLBACK_PLUS,
    CALLBACK_REACHABILITY
};
#define CALLBACK_COUNT 37
typedef struct {
    void *function, *argument;
    uint32_t generation, in_flight;
    int active, kind;
} StateCallback;
typedef struct StateEvent {
    struct StateEvent *next;
    PsnEvent value;
    uint32_t targets[CALLBACK_COUNT];
} StateEvent;
static StateCallback callbacks[CALLBACK_COUNT];
static StateEvent *event_head, *event_tail;
static unsigned queued_events;
static uint32_t callback_generation;
static int dispatching;
static _Thread_local StateCallback *current_callback;
typedef void(PSN_GUEST_ABI *StateLegacy)(int32_t, int32_t, const GuestNpId *, void *);
typedef void(PSN_GUEST_ABI *StateModern)(int32_t, int32_t, void *);
typedef void(PSN_GUEST_ABI *PresenceCallback)(const GuestOnlineId *, void *);

static void callback_slots(int kind, int *first, int *end) {
    static const int first_slots[] = {0, 1, 2, 18, 19, 35, 36};
    static const int end_slots[] = {1, 2, 18, 19, 35, 36, 37};
    *first = first_slots[kind];
    *end = end_slots[kind];
}
static int callback_matches_event(int callback_kind, uint32_t event_kind) {
    if (event_kind == 1)
        return callback_kind <= CALLBACK_STATE_A;
    if (event_kind == 4)
        return callback_kind == CALLBACK_PRESENCE_LEGACY || callback_kind == CALLBACK_PRESENCE_A;
    if (event_kind == 5)
        return callback_kind == CALLBACK_PLUS;
    return event_kind == 6 && callback_kind == CALLBACK_REACHABILITY;
}
static int32_t queue_event_locked(const PsnEvent *value, int only_slot) {
    int targets = 0;
    for (int i = 0; i < CALLBACK_COUNT; ++i)
        if (callbacks[i].active && callback_matches_event(callbacks[i].kind, value->kind) &&
            (only_slot < 0 || i == only_slot))
            ++targets;
    if (!targets)
        return 0;
    if (queued_events >= 256)
        return PSN_BACKEND_BUSY;
    StateEvent *event = calloc(1, sizeof(*event));
    if (!event)
        return PSN_BACKEND_NO_MEMORY;
    event->value = *value;
    for (int i = 0; i < CALLBACK_COUNT; ++i)
        if (callbacks[i].active && callback_matches_event(callbacks[i].kind, value->kind) &&
            (only_slot < 0 || i == only_slot)) {
            event->targets[i] = callbacks[i].generation;
        }
    if (event_tail)
        event_tail->next = event;
    else
        event_head = event;
    event_tail = event;
    ++queued_events;
    return 0;
}
int32_t psn_host_identity_event(const PsnEvent *event) {
    if (!event || !psn_host_has_local_user(event->user_id))
        return PSN_INVALID_ARGUMENT;
    switch (event->kind) {
    case 1:
        if (!memchr(event->data.identity.online_id, 0, 17) || event->data.identity.state < 1 ||
            event->data.identity.state > 2)
            return PSN_INVALID_ARGUMENT;
        break;
    case 4:
        if (!memchr(event->data.online_id, 0, 17))
            return PSN_INVALID_ARGUMENT;
        break;
    case 5:
        break; /* The backend supplies the PS4 Plus event value unchanged. */
    case 6:
        if (event->data.reachability < 0 || event->data.reachability > 2)
            return PSN_INVALID_ARGUMENT;
        break;
    default:
        return PSN_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&identity_mutex);
    int32_t rc = queue_event_locked(event, -1);
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static int32_t register_state(void *function, void *argument, int kind) {
    if (!function)
        return PSN_INVALID_ARGUMENT;
    pthread_mutex_lock(&identity_mutex);
    int first, end, slot = -1;
    callback_slots(kind, &first, &end);
    for (int i = first; i < end; ++i)
        if (!callbacks[i].active && !callbacks[i].in_flight) {
            slot = i;
            break;
        }
    int32_t rc = end - first > 1 ? NP_CALLBACK_MAX : NP_CALLBACK_REGISTERED;
    if (slot >= 0 && !callbacks[slot].active && !callbacks[slot].in_flight &&
        callback_generation < INT_MAX) {
        StateCallback *cb = &callbacks[slot];
        cb->function = function;
        cb->argument = argument;
        cb->active = 1;
        cb->kind = kind;
        cb->generation = ++callback_generation;
        rc = 0;
        int32_t users[PSN_MAX_LOCAL_USERS];
        uint32_t count = psn_host_local_users(users);
        for (uint32_t i = 0; kind <= CALLBACK_STATE_A && i < count; ++i) {
            int32_t user = users[i];
            PsnIdentity identity = {0};
            if (!get_identity(user, &identity)) {
                PsnEvent event = {.kind = 1, .user_id = user};
                event.data.identity = identity;
                rc = queue_event_locked(&event, slot);
                if (rc)
                    break;
            }
        }
        if (rc)
            cb->active = 0;
        else
            rc = end - first > 1 ? (int32_t)cb->generation : 0;
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static int32_t unregister_state(int kind, int32_t id) {
    pthread_mutex_lock(&identity_mutex);
    int first, end;
    callback_slots(kind, &first, &end);
    StateCallback *cb = end - first == 1 ? &callbacks[first] : NULL;
    if (end - first > 1)
        for (int i = first; i < end; ++i)
            if (callbacks[i].active && callbacks[i].generation == (uint32_t)id) {
                cb = &callbacks[i];
                break;
            }
    int32_t rc = NP_CALLBACK_MISSING;
    if (cb && cb->active) {
        cb->active = 0;
        rc = 0;
        while (cb->in_flight && cb != current_callback)
            pthread_cond_wait(&callback_idle, &identity_mutex);
        /* The dispatcher retires the old callback. A waiter must not clear a new
         * registration that acquired the same slot before it regained this lock. */
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static PSN_GUEST_ABI int32_t state_register(void *cb, void *arg) {
    return register_state(cb, arg, 0);
}
static PSN_GUEST_ABI int32_t state_register_toolkit(void *cb, void *arg) {
    return register_state(cb, arg, 1);
}
static PSN_GUEST_ABI int32_t state_register_a(void *cb, void *arg) {
    return register_state(cb, arg, 2);
}
static PSN_GUEST_ABI int32_t state_unregister(void) { return unregister_state(0, 0); }
static PSN_GUEST_ABI int32_t state_unregister_toolkit(void) { return unregister_state(1, 0); }
static PSN_GUEST_ABI int32_t state_unregister_a(int32_t id) { return unregister_state(2, id); }
static PSN_GUEST_ABI void presence_register(void *function, void *argument) {
    /* The legacy API is void and replaces its one callback; NULL clears it. */
    pthread_mutex_lock(&identity_mutex);
    StateCallback *cb = &callbacks[18];
    for (;;) {
        /* A competing replacement may have acquired this slot while we waited.
         * Quiesce that delivery as well before replacing its user argument. */
        cb->active = 0;
        if (!cb->in_flight || cb == current_callback)
            break;
        pthread_cond_wait(&callback_idle, &identity_mutex);
    }
    if (function && callback_generation < INT_MAX) {
        cb->function = function;
        cb->argument = argument;
        cb->active = 1;
        cb->kind = CALLBACK_PRESENCE_LEGACY;
        cb->generation = ++callback_generation;
    } else if (function)
        fprintf(stderr, "[psn-host] GamePresence callback handle limit reached\n");
    else if (!cb->in_flight) {
        cb->function = NULL;
        cb->argument = NULL;
    }
    pthread_mutex_unlock(&identity_mutex);
}
static PSN_GUEST_ABI int32_t presence_register_a(void *cb, void *arg) {
    return register_state(cb, arg, CALLBACK_PRESENCE_A);
}
static PSN_GUEST_ABI int32_t presence_unregister_a(int32_t id) {
    return unregister_state(CALLBACK_PRESENCE_A, id);
}
static PSN_GUEST_ABI int32_t plus_register(void *cb, void *arg) {
    return register_state(cb, arg, CALLBACK_PLUS);
}
static PSN_GUEST_ABI int32_t plus_unregister(void) { return unregister_state(CALLBACK_PLUS, 0); }
static PSN_GUEST_ABI int32_t reachability_register(void *cb, void *arg) {
    return register_state(cb, arg, CALLBACK_REACHABILITY);
}
static PSN_GUEST_ABI int32_t reachability_unregister(void) {
    return unregister_state(CALLBACK_REACHABILITY, 0);
}
static PSN_GUEST_ABI int32_t check_callback(void) {
    int32_t rc = psn_host_pump();
    psn_host_identity_dispatch();
    psn_host_webapi_dispatch();
    return rc;
}
void psn_host_identity_dispatch(void) {
    pthread_mutex_lock(&identity_mutex);
    if (dispatching) {
        pthread_mutex_unlock(&identity_mutex);
        return;
    }
    dispatching = 1;
    /* One finite batch: callbacks may enqueue the next batch reentrantly. */
    StateEvent *batch = event_head;
    event_head = event_tail = NULL;
    while (batch) {
        StateEvent *event = batch;
        batch = event->next;
        for (int i = 0; i < CALLBACK_COUNT; ++i) {
            StateCallback *cb = &callbacks[i];
            if (!event->targets[i] || !cb->active || event->targets[i] != cb->generation)
                continue;
            ++cb->in_flight;
            void *function = cb->function, *argument = cb->argument;
            int kind = cb->kind;
            pthread_mutex_unlock(&identity_mutex);
            current_callback = cb;
            if (kind == CALLBACK_STATE_LEGACY) {
                GuestNpId npid = {0};
                online_to_guest(&npid.online, event->value.data.identity.online_id);
                ((StateLegacy)function)(event->value.user_id, event->value.data.identity.state,
                                        event->value.data.identity.state == 2 ? &npid : NULL,
                                        argument);
            } else if (kind <= CALLBACK_STATE_A)
                ((StateModern)function)(event->value.user_id, event->value.data.identity.state,
                                        argument);
            else if (kind == CALLBACK_PRESENCE_LEGACY || kind == CALLBACK_PRESENCE_A) {
                GuestOnlineId online;
                online_to_guest(&online, event->value.data.online_id);
                ((PresenceCallback)function)(&online, argument);
            } else if (kind == CALLBACK_PLUS)
                ((StateModern)function)(event->value.user_id, event->value.data.plus_event,
                                        argument);
            else
                ((StateModern)function)(event->value.user_id, event->value.data.reachability,
                                        argument);
            current_callback = NULL;
            pthread_mutex_lock(&identity_mutex);
            --cb->in_flight;
            if (!cb->active && !cb->in_flight) {
                cb->function = NULL;
                cb->argument = NULL;
            }
            pthread_cond_broadcast(&callback_idle);
        }
        free(event);
        --queued_events;
    }
    dispatching = 0;
    pthread_cond_broadcast(&callback_idle);
    pthread_mutex_unlock(&identity_mutex);
}

typedef struct {
    char data[45], term, padding[2];
} GuestSessionId;
typedef union {
    struct {
        const void *users;
        uint32_t count, padding;
    } fixed;
    uint32_t maximum;
} GuestAddress;
typedef struct {
    int32_t type;
    uint32_t padding;
    GuestAddress address;
} GuestAddressParam;
typedef union {
    struct {
        const char *message;
        const GuestSessionId *session;
        GuestAddressParam address;
    } send;
    uint8_t receive[64];
} GuestDialogData;
typedef struct {
    uint64_t base_size;
    uint8_t base_reserved[36];
    uint32_t magic;
    uint32_t size;
    int32_t mode, user;
    uint32_t padding;
    void *callback_argument;
    const GuestDialogData *data;
    uint8_t reserved[64];
} GuestDialogParam;
typedef struct {
    GuestOnlineId online;
    uint32_t padding;
    uint64_t account;
} GuestDialogUser;
typedef struct {
    uint32_t count, padding;
    GuestDialogUser users[16];
} GuestDialogUsers;
typedef struct {
    uint32_t count;
    GuestOnlineId online[16];
} GuestDialogOnlineUsers;
typedef struct {
    void *callback_argument;
    int32_t error, result;
    void *users;
    uint8_t reserved[32];
} GuestDialogResult;
_Static_assert(sizeof(GuestDialogParam) == 144, "invitation parameter layout");
_Static_assert(offsetof(GuestDialogParam, data) == 72, "invitation data offset");
_Static_assert(sizeof(GuestDialogResult) == 56, "invitation result layout");
_Static_assert(sizeof(GuestDialogUser) == 32, "invitation user layout");

static struct {
    int status, backend_started;
    uint64_t handle;
    void *callback_argument;
    PsnInvitationResult result;
} dialog;
static void dialog_release(void) {
    if (!dialog.backend_started)
        return;
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    if (backend) {
        if (dialog.status == 2)
            backend->invitation_cancel(instance, dialog.handle);
        backend->invitation_release(instance, dialog.handle);
    }
    psn_host_unlock();
    dialog.backend_started = 0;
}
static PSN_GUEST_ABI int32_t dialog_initialize(void) {
    pthread_mutex_lock(&identity_mutex);
    int32_t rc = dialog.status ? DIALOG_ERROR(4) : 0;
    if (!rc)
        dialog.status = 1;
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static int32_t dialog_open(const GuestDialogParam *param, int account_ids) {
    if (!param)
        return DIALOG_ERROR(13);
    GuestDialogParam copied = *param;
    if (copied.size != sizeof(copied) || copied.base_size != 48 ||
        !psn_host_has_local_user(copied.user) || (copied.mode != 1 && copied.mode != 2))
        return DIALOG_ERROR(10);
    char message[513] = {0}, session[46] = {0};
    PsnInvitationUser users[16] = {{0}};
    PsnInvitationRequest input = {0};
    input.user_id = copied.user;
    input.mode = (uint32_t)copied.mode;
    input.session_id = session;
    input.message = message;
    input.users = users;
    if (copied.mode == 1) {
        if (!copied.data)
            return DIALOG_ERROR(10);
        GuestDialogData data = *copied.data;
        if (!data.send.session || !copy_string(session, sizeof(session), data.send.session->data) ||
            !session[0] ||
            (data.send.message && !copy_string(message, sizeof(message), data.send.message)))
            return DIALOG_ERROR(10);
        if (data.send.address.type == 1) {
            uint32_t count = data.send.address.address.fixed.count;
            const void *source = data.send.address.address.fixed.users;
            if (!source || !count || count > 16)
                return DIALOG_ERROR(10);
            input.user_count = count;
            for (uint32_t i = 0; i < count; ++i) {
                if (account_ids) {
                    users[i].account_id = ((const uint64_t *)source)[i];
                    if (!users[i].account_id)
                        return DIALOG_ERROR(10);
                } else {
                    GuestOnlineId online = ((const GuestOnlineId *)source)[i];
                    if (online.term)
                        return DIALOG_ERROR(10);
                    memcpy(users[i].online_id, online.data, 16);
                }
            }
        } else if (data.send.address.type == 2) {
            input.max_selectable_users = data.send.address.address.maximum;
            if (!input.max_selectable_users || input.max_selectable_users > 16)
                return DIALOG_ERROR(10);
        } else
            return DIALOG_ERROR(10);
    }
    pthread_mutex_lock(&identity_mutex);
    int32_t rc = 0;
    if (!dialog.status)
        rc = DIALOG_ERROR(3);
    else if (dialog.status == 2)
        rc = DIALOG_ERROR(8);
    else {
        dialog_release();
        void *instance;
        const PsnBackendV1 *backend = psn_host_lock(&instance);
        uint64_t handle = 0;
        rc = backend ? backend->invitation_start(instance, &input, &handle)
                     : PSN_BACKEND_UNAVAILABLE;
        psn_host_unlock();
        if (!rc) {
            memset(&dialog.result, 0, sizeof(dialog.result));
            dialog.handle = handle;
            dialog.backend_started = 1;
            dialog.status = 2;
            dialog.callback_argument = copied.callback_argument;
        }
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static PSN_GUEST_ABI int32_t dialog_open_a(const GuestDialogParam *param) {
    return dialog_open(param, 1);
}
static PSN_GUEST_ABI int32_t dialog_open_legacy(const GuestDialogParam *param) {
    return dialog_open(param, 0);
}
static void dialog_poll(void) {
    if (dialog.status != 2)
        return;
    PsnInvitationResult result = {0};
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    int32_t rc = backend ? backend->invitation_poll(instance, dialog.handle, &result)
                         : PSN_BACKEND_UNAVAILABLE;
    psn_host_unlock();
    if (rc == 1)
        return;
    if (rc || result.user_count > 16 || (result.result != 0 && result.result != 1)) {
        memset(&result, 0, sizeof(result));
        result.error_code = rc < 0 ? rc : PSN_BACKEND_UNAVAILABLE;
        result.result = 1;
    }
    for (uint32_t i = 0; i < result.user_count; ++i)
        if (!memchr(result.users[i].online_id, 0, sizeof(result.users[i].online_id))) {
            result.error_code = PSN_BACKEND_UNAVAILABLE;
            result.result = 1;
            result.user_count = 0;
            break;
        }
    if (result.error_code || result.result)
        result.user_count = 0;
    dialog.result = result;
    dialog.status = 3;
}
static PSN_GUEST_ABI int32_t dialog_status(void) {
    pthread_mutex_lock(&identity_mutex);
    dialog_poll();
    int32_t status = dialog.status;
    pthread_mutex_unlock(&identity_mutex);
    return status;
}
static int32_t dialog_result(GuestDialogResult *out, int account_ids) {
    if (!out)
        return DIALOG_ERROR(13);
    pthread_mutex_lock(&identity_mutex);
    int32_t rc = !dialog.status ? DIALOG_ERROR(3) : dialog.status != 3 ? DIALOG_ERROR(5) : 0;
    if (!rc) {
        out->callback_argument = dialog.callback_argument;
        out->error = dialog.result.error_code;
        out->result = dialog.result.result;
        memset(out->reserved, 0, sizeof(out->reserved));
        if (out->users) {
            if (account_ids) {
                GuestDialogUsers *list = out->users;
                memset(list, 0, sizeof(*list));
                list->count = dialog.result.user_count;
                for (uint32_t i = 0; i < list->count; ++i) {
                    online_to_guest(&list->users[i].online, dialog.result.users[i].online_id);
                    list->users[i].account = dialog.result.users[i].account_id;
                }
            } else {
                GuestDialogOnlineUsers *list = out->users;
                memset(list, 0, sizeof(*list));
                list->count = dialog.result.user_count;
                for (uint32_t i = 0; i < list->count; ++i)
                    online_to_guest(&list->online[i], dialog.result.users[i].online_id);
            }
        }
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static PSN_GUEST_ABI int32_t dialog_result_a(GuestDialogResult *out) {
    return dialog_result(out, 1);
}
static PSN_GUEST_ABI int32_t dialog_result_legacy(GuestDialogResult *out) {
    return dialog_result(out, 0);
}
static PSN_GUEST_ABI int32_t dialog_close(void) {
    pthread_mutex_lock(&identity_mutex);
    int32_t rc = !dialog.status ? DIALOG_ERROR(3) : dialog.status != 2 ? DIALOG_ERROR(11) : 0;
    if (!rc) {
        dialog_release();
        memset(&dialog.result, 0, sizeof(dialog.result));
        dialog.result.result = 1;
        dialog.status = 3;
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}
static PSN_GUEST_ABI int32_t dialog_terminate(void) {
    pthread_mutex_lock(&identity_mutex);
    int32_t rc = !dialog.status ? DIALOG_ERROR(3) : 0;
    if (!rc) {
        dialog_release();
        memset(&dialog, 0, sizeof(dialog));
    }
    pthread_mutex_unlock(&identity_mutex);
    return rc;
}

void psn_host_identity_reset(void) {
    pthread_mutex_lock(&identity_mutex);
    for (int i = 0; i < CALLBACK_COUNT; ++i)
        callbacks[i].active = 0;
    while (dispatching && !current_callback)
        pthread_cond_wait(&callback_idle, &identity_mutex);
    for (int i = 0; i < CALLBACK_COUNT; ++i)
        if (!callbacks[i].in_flight)
            memset(&callbacks[i], 0, sizeof(callbacks[i]));
    while (event_head) {
        StateEvent *next = event_head->next;
        free(event_head);
        event_head = next;
        --queued_events;
    }
    event_tail = NULL;
    for (size_t i = 0; i < 48; ++i)
        if (requests[i].id)
            request_release(&requests[i]);
    dialog_release();
    memset(&dialog, 0, sizeof(dialog));
    pthread_mutex_unlock(&identity_mutex);
}

static const PsnHostExport identity_exports[] = {
    {"sceNpGetState", np_state},
    {"sceNpGetOnlineId", np_online},
    {"sceNpGetNpId", np_id},
    {"sceNpGetAccountIdA", np_account},
    {"sceNpGetUserIdByAccountId", np_user},
    {"sceNpSetNpTitleId", set_title},
    {"sceNpSetContentRestriction", set_restriction},
    {"sceNpCmpOnlineId", compare_online},
    {"sceNpCmpNpId", compare_id},
    {"sceNpCreateRequest", np_create},
    {"sceNpCreateAsyncRequest", np_create_async},
    {"sceNpDeleteRequest", np_delete},
    {"sceNpAbortRequest", np_abort},
    {"sceNpPollAsync", np_poll},
    {"sceNpWaitAsync", np_wait},
    {"sceNpCheckNpAvailability", np_available},
    {"sceNpCheckNpAvailabilityA", np_available_a},
    {"sceNpCheckPlus", np_plus},
    {"sceNpGetParentalControlInfo", np_parental},
    {"sceNpGetParentalControlInfoA", np_parental_a},
    {"sceNpAuthCreateRequest", auth_create},
    {"sceNpAuthCreateAsyncRequest", auth_create_async},
    {"sceNpAuthDeleteRequest", auth_delete},
    {"sceNpAuthAbortRequest", auth_abort},
    {"sceNpAuthPollAsync", auth_poll},
    {"sceNpAuthWaitAsync", auth_wait},
    {"sceNpAuthSetTimeout", auth_timeout},
    {"sceNpAuthGetIdToken", auth_token},
    {"sceNpAuthGetIdTokenA", auth_token_a},
    {"sceNpAuthGetIdTokenV3", auth_token_v3},
    {"sceNpAuthGetAuthorizationCode", auth_code},
    {"sceNpAuthGetAuthorizationCodeA", auth_code_a},
    {"sceNpAuthGetAuthorizationCodeV3", auth_code_v3},
    {"sceNpRegisterStateCallback", state_register},
    {"sceNpUnregisterStateCallback", state_unregister},
    {"sceNpRegisterStateCallbackA", state_register_a},
    {"sceNpUnregisterStateCallbackA", state_unregister_a},
    {"sceNpRegisterStateCallbackForToolkit", state_register_toolkit},
    {"sceNpUnregisterStateCallbackForToolkit", state_unregister_toolkit},
    {"sceNpRegisterGamePresenceCallback", presence_register},
    {"sceNpRegisterGamePresenceCallbackA", presence_register_a},
    {"sceNpUnregisterGamePresenceCallbackA", presence_unregister_a},
    {"sceNpRegisterPlusEventCallback", plus_register},
    {"sceNpUnregisterPlusEventCallback", plus_unregister},
    {"sceNpRegisterNpReachabilityStateCallback", reachability_register},
    {"sceNpUnregisterNpReachabilityStateCallback", reachability_unregister},
    {"sceNpCheckCallback", check_callback},
    {"sceNpCheckCallbackForLib", check_callback},
    {"sceInvitationDialogInitialize", dialog_initialize},
    {"sceInvitationDialogOpen", dialog_open_legacy},
    {"sceInvitationDialogOpenA", dialog_open_a},
    {"sceInvitationDialogGetStatus", dialog_status},
    {"sceInvitationDialogUpdateStatus", dialog_status},
    {"sceInvitationDialogGetResult", dialog_result_legacy},
    {"sceInvitationDialogGetResultA", dialog_result_a},
    {"sceInvitationDialogClose", dialog_close},
    {"sceInvitationDialogTerminate", dialog_terminate},
};
uintptr_t psn_host_identity_resolve(const char *name) { return PSN_LOOKUP(identity_exports, name); }
