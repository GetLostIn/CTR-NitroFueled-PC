/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-only contract checks. No game, platform client or network connection. */
#include "psn_host.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int32_t test_user = 1;
uint32_t psn_host_local_users(int32_t users[PSN_MAX_LOCAL_USERS]) {
    users[0] = test_user;
    return 1;
}
int psn_host_has_local_user(int32_t user) { return user == test_user; }

static pthread_mutex_t backend_mutex = PTHREAD_MUTEX_INITIALIZER;
static int backend_locked, auth_pending, auth_canceled, auth_released, dialog_pending;
static int dialog_canceled, dialog_released, web_dispatches, auth_failure;
static int auth_started;
static uint32_t auth_variant, auth_kind;
static int32_t policy_failure;
static int32_t identity_state = 2;
static uint64_t checked_features;
static const char *guest_client;
static PsnInvitationResult invitation_result;
static PsnAuthTimeout seen_timeout;
static char seen_title[13];

static int32_t identity(void *ctx, int32_t user, PsnIdentity *out) {
    (void)ctx;
    assert(backend_locked);
    if (user != test_user)
        return (int32_t)0x80550007;
    *out = (PsnIdentity){0};
    out->account_id = 0x123456789;
    memcpy(out->online_id, "abcdefghijklmnop", 17);
    out->state = identity_state;
    return 0;
}
static int32_t policy(void *ctx, int32_t user, uint64_t features, PsnPolicy *out) {
    (void)ctx;
    assert(backend_locked);
    assert(user == test_user);
    checked_features = features;
    if (policy_failure)
        return policy_failure;
    *out = (PsnPolicy){0};
    out->age = 13;
    out->plus_authorized = 0;
    out->chat_restricted = 1;
    return 0;
}
static int32_t title(void *ctx, const char *value, PsnBytes secret) {
    (void)ctx;
    assert(backend_locked);
    assert(secret.size == 128);
    assert(((const unsigned char *)secret.data)[127] == 23);
    strcpy(seen_title, value);
    return 0;
}
static int32_t restriction(void *ctx, const PsnContentRestriction *r) {
    (void)ctx;
    assert(backend_locked);
    assert(r->default_age == 7);
    assert(r->count == 1 && r->countries[0].age == 12 && !strcmp(r->countries[0].country, "IT"));
    return 0;
}
static int32_t auth_start(void *ctx, const PsnAuthRequest *input, uint64_t *handle) {
    (void)ctx;
    assert(backend_locked);
    assert(input->user_id == test_user);
    assert(input->client_id != guest_client);
    assert(!strcmp(input->client_id, "client"));
    assert(!strcmp(input->scope, "openid"));
    auth_variant = input->api_variant;
    auth_kind = input->kind;
    if (input->has_timeout)
        seen_timeout = input->timeout;
    ++auth_started;
    *handle = 91;
    return auth_failure;
}
static int32_t auth_poll(void *ctx, uint64_t handle, PsnAuthResult *out) {
    (void)ctx;
    assert(backend_locked);
    assert(handle == 91);
    assert(!auth_canceled);
    if (auth_pending) {
        --auth_pending;
        return 1;
    }
    *out = (PsnAuthResult){0};
    strcpy(out->value, auth_kind == 1 ? "provider-token" : "provider-code");
    out->issuer_id = 77;
    return 0;
}
static void auth_cancel(void *ctx, uint64_t handle) {
    (void)ctx;
    assert(backend_locked);
    assert(handle == 91);
    ++auth_canceled;
}
static void auth_release(void *ctx, uint64_t handle) {
    (void)ctx;
    assert(backend_locked);
    assert(handle == 91);
    ++auth_released;
}
static int32_t invite_start(void *ctx, const PsnInvitationRequest *input, uint64_t *handle) {
    (void)ctx;
    assert(backend_locked);
    assert(input->user_id == test_user);
    if (input->mode == 1) {
        assert(!strcmp(input->session_id, "opaque-session"));
        assert(!strcmp(input->message, "Join"));
        assert(input->user_count == 1 && input->users[0].account_id == 43);
    } else
        assert(input->mode == 2 && input->user_count == 0);
    *handle = 88;
    return 0;
}
static int32_t invite_poll(void *ctx, uint64_t handle, PsnInvitationResult *out) {
    (void)ctx;
    assert(backend_locked);
    assert(handle == 88);
    if (dialog_pending)
        return 1;
    *out = invitation_result;
    return 0;
}
static void invite_cancel(void *ctx, uint64_t handle) {
    (void)ctx;
    assert(backend_locked);
    assert(handle == 88);
    ++dialog_canceled;
}
static void invite_release(void *ctx, uint64_t handle) {
    (void)ctx;
    assert(backend_locked);
    assert(handle == 88);
    ++dialog_released;
}
static const PsnBackendV1 backend = {.identity = identity,
                                     .policy = policy,
                                     .set_title = title,
                                     .set_content_restriction = restriction,
                                     .auth_start = auth_start,
                                     .auth_poll = auth_poll,
                                     .auth_cancel = auth_cancel,
                                     .auth_release = auth_release,
                                     .invitation_start = invite_start,
                                     .invitation_poll = invite_poll,
                                     .invitation_cancel = invite_cancel,
                                     .invitation_release = invite_release};
const PsnBackendV1 *psn_host_lock(void **instance) {
    pthread_mutex_lock(&backend_mutex);
    assert(!backend_locked);
    backend_locked = 1;
    *instance = NULL;
    return &backend;
}
void psn_host_unlock(void) {
    assert(backend_locked);
    backend_locked = 0;
    pthread_mutex_unlock(&backend_mutex);
}
int32_t psn_host_pump(void) {
    assert(!backend_locked);
    return 0;
}
void psn_host_webapi_dispatch(void) {
    assert(!backend_locked);
    ++web_dispatches;
}
typedef int32_t(PSN_GUEST_ABI *VoidFn)(void);
typedef int32_t(PSN_GUEST_ABI *IdFn)(int32_t);
typedef int32_t(PSN_GUEST_ABI *PointerFn)(const void *);
typedef int32_t(PSN_GUEST_ABI *PollFn)(int32_t, int32_t *);
typedef int32_t(PSN_GUEST_ABI *UserOutFn)(int32_t, void *);
typedef int32_t(PSN_GUEST_ABI *AuthFn)(int32_t, const void *, void *, int32_t *);
typedef int32_t(PSN_GUEST_ABI *RegisterFn)(void *, void *);
typedef int32_t(PSN_GUEST_ABI *TimeoutFn)(int32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uintptr_t resolve(const char *name) {
    uintptr_t function = psn_host_identity_resolve(name);
    assert(function);
    return function;
}
#define CALL0(name) ((VoidFn)resolve(name))()
#define CALLID(name, id) ((IdFn)resolve(name))(id)
#define CALLPTR(name, p) ((PointerFn)resolve(name))(p)
#define POLL(name, id, p) ((PollFn)resolve(name))(id, p)

typedef struct {
    uint64_t size, affinity;
    int32_t priority;
    uint32_t padding;
} Async;
typedef struct {
    uint64_t size;
    int32_t user;
    uint32_t padding;
    const char *client, *secret, *scope;
} Auth;
static void test_auth(void) {
    Async async = {sizeof(async), 0, 0, 0};
    Auth param = {sizeof(param), test_user, 0, "client", "secret", "openid"};
    guest_client = param.client;
    int32_t request = CALLPTR("sceNpAuthCreateAsyncRequest", &async);
    assert(request > 0);
    struct {
        unsigned char before;
        char value[4104];
        unsigned char after;
    } output;
    memset(&output, 0xcc, sizeof(output));
    int32_t result = 0x23456789;
    assert(POLL("sceNpAuthPollAsync", request, &result) == 1);
    assert(result == 0x23456789);
    assert(((TimeoutFn)resolve("sceNpAuthSetTimeout"))(request, 2, 3, 4, 5, 6) == 0);
    auth_pending = 1;
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenV3"))(request, &param, output.value, NULL) == 0);
    assert(auth_variant == 3 && seen_timeout.resolve_retry == 2 &&
           seen_timeout.receive_timeout == 6);
    assert(((TimeoutFn)resolve("sceNpAuthSetTimeout"))(request, 2, 3, 4, 5, 6) ==
           PSN_BACKEND_UNSUPPORTED);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 1);
    assert(result == 0x23456789 && (unsigned char)output.value[0] == 0xcc);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == 0);
    assert(!strcmp(output.value, "provider-token") && output.before == 0xcc &&
           output.after == 0xcc);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0 && auth_released == 1);
    assert(POLL("sceNpAuthPollAsync", request, &result) == (int32_t)0x80550306);

    request = CALLPTR("sceNpAuthCreateAsyncRequest", &async);
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, output.value, NULL) == 0);
    memset(output.value, 0xcc, sizeof(output.value));
    assert(CALLID("sceNpAuthAbortRequest", request) == 0 && auth_canceled == 1);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == (int32_t)0x80550304);
    assert((unsigned char)output.value[0] == 0xcc);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0 && auth_released == 2);
    auth_canceled = 0;

    request = CALL0("sceNpAuthCreateRequest");
    assert(POLL("sceNpAuthPollAsync", request, &result) == (int32_t)0x80550307);
    struct {
        uint64_t size;
        int32_t user;
        uint32_t padding;
        const char *client, *scope;
    } code_param = {32, test_user, 0, "client", "openid"};
    char code[136];
    int32_t issuer = 0;
    auth_pending = 1;
    assert(((AuthFn)resolve("sceNpAuthGetAuthorizationCodeA"))(request, &code_param, code,
                                                               &issuer) == 0);
    assert(!strcmp(code, "provider-code") && issuer == 77 && auth_variant == 1);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0);

    request = CALLPTR("sceNpAuthCreateAsyncRequest", &async);
    auth_failure = PSN_SIGNED_OUT;
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, output.value, NULL) == 0);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == PSN_SIGNED_OUT);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0);
    auth_failure = 0;
}
static void test_repeated_auth_get(void) {
    Async async = {sizeof(async), 0, 0, 0};
    Auth param = {sizeof(param), test_user, 0, "client", "secret", "openid"};
    guest_client = param.client;
    unsigned char first[4104], second[4104];
    memset(first, 0xa5, sizeof(first));
    memset(second, 0x5a, sizeof(second));
    int started_before = auth_started, released_before = auth_released;
    int32_t request = CALLPTR("sceNpAuthCreateAsyncRequest", &async), result = 19;
    auth_pending = 0;
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, first, NULL) == 0);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == 0);
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, second, NULL) ==
           (int32_t)0x80550301);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == (int32_t)0x80550301);
    assert(!strcmp((char *)first, "provider-token") && auth_started == started_before + 1);
    for (size_t i = 0; i < sizeof(second); ++i)
        assert(second[i] == 0x5a);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0 && auth_released == released_before + 1);

    request = CALLPTR("sceNpAuthCreateAsyncRequest", &async);
    memset(first, 0xa5, sizeof(first));
    auth_pending = 1;
    result = 19;
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, first, NULL) == 0);
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, second, NULL) ==
           (int32_t)0x80550301);
    /* Rejection is not completion: Poll cannot release the first output buffer. */
    assert(POLL("sceNpAuthPollAsync", request, &result) == 1 && result == 19);
    for (size_t i = 0; i < sizeof(first); ++i)
        assert(first[i] == 0xa5);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == 0);
    assert(!strcmp((char *)first, "provider-token") && auth_started == started_before + 2);
    for (size_t i = 0; i < sizeof(second); ++i)
        assert(second[i] == 0x5a);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0 && auth_released == released_before + 2);

    request = CALLPTR("sceNpAuthCreateAsyncRequest", &async);
    memset(first, 0xa5, sizeof(first));
    auth_pending = 1;
    auth_canceled = 0;
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, first, NULL) == 0);
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, second, NULL) ==
           (int32_t)0x80550301);
    assert(CALLID("sceNpAuthAbortRequest", request) == 0 && auth_canceled == 1);
    assert(((AuthFn)resolve("sceNpAuthGetIdTokenA"))(request, &param, second, NULL) ==
           (int32_t)0x80550304);
    assert(POLL("sceNpAuthPollAsync", request, &result) == 0 && result == (int32_t)0x80550304);
    assert(CALLID("sceNpAuthDeleteRequest", request) == 0 && auth_released == released_before + 3);
    for (size_t i = 0; i < sizeof(first); ++i)
        assert(first[i] == 0xa5);
    for (size_t i = 0; i < sizeof(second); ++i)
        assert(second[i] == 0x5a);
    assert(auth_started == started_before + 3);
    auth_canceled = 0;
    auth_pending = 0;
}

static int state_calls, modern_calls;
static int legacy_transitions;
static PSN_GUEST_ABI void legacy_transition_callback(int32_t user, int32_t state, const void *npid,
                                                     void *argument) {
    assert(user == test_user && argument == &legacy_transitions);
    if (state == 2)
        assert(npid && !memcmp(npid, "abcdefghijklmnop", 16));
    else
        assert(state == 1 && !npid);
    ++legacy_transitions;
}
static PSN_GUEST_ABI void state_callback(int32_t user, int32_t state, const void *npid,
                                         void *argument) {
    assert(!backend_locked && user == test_user && state == 2 && argument == &state_calls);
    assert(!memcmp(npid, "abcdefghijklmnop", 16));
    ++state_calls;
    assert(CALL0("sceNpUnregisterStateCallback") == 0);
    assert(CALL0("sceNpCheckCallback") == 0); /* reentrant pump never redelivers this callback */
}
static PSN_GUEST_ABI void modern_callback(int32_t user, int32_t state, void *argument) {
    assert(!backend_locked && user == test_user && (state == 1 || state == 2));
    assert(argument == &modern_calls);
    ++modern_calls;
}
static void test_callbacks(void) {
    assert(((RegisterFn)resolve("sceNpRegisterStateCallback"))(state_callback, &state_calls) == 0);
    int32_t handle =
        ((RegisterFn)resolve("sceNpRegisterStateCallbackA"))(modern_callback, &modern_calls);
    assert(handle > 0 && !state_calls && !modern_calls);
    assert(CALL0("sceNpCheckCallbackForLib") == 0);
    assert(state_calls == 1 && modern_calls == 1 && web_dispatches == 2);
    PsnEvent event = {0};
    event.kind = 1;
    event.user_id = test_user;
    event.data.identity.state = 1;
    psn_host_identity_event(&event);
    assert(modern_calls == 1);
    assert(CALL0("sceNpCheckCallback") == 0 && modern_calls == 2);
    psn_host_identity_event(&event);
    assert(CALLID("sceNpUnregisterStateCallbackA", handle) == 0);
    assert(CALL0("sceNpCheckCallback") == 0 && modern_calls == 2);
    assert(CALLID("sceNpUnregisterStateCallbackA", handle) == (int32_t)0x80550009);
    handle = ((RegisterFn)resolve("sceNpRegisterStateCallbackA"))(modern_callback, &modern_calls);
    assert(handle > 0);
    assert(CALL0("sceNpCheckCallback") == 0);
    for (int i = 0; i < 256; ++i)
        assert(psn_host_identity_event(&event) == 0);
    assert(psn_host_identity_event(&event) == PSN_BACKEND_BUSY);
    assert(CALLID("sceNpUnregisterStateCallbackA", handle) == 0);
    assert(CALL0("sceNpCheckCallback") == 0);
    assert(((RegisterFn)resolve("sceNpRegisterStateCallback"))(legacy_transition_callback,
                                                               &legacy_transitions) == 0);
    assert(psn_host_identity_event(&event) == 0);
    assert(CALL0("sceNpCheckCallback") == 0 && legacy_transitions == 2);
    assert(CALL0("sceNpUnregisterStateCallback") == 0);
}

static pthread_mutex_t callback_test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_test_condition = PTHREAD_COND_INITIALIZER;
static int callback_entered, callback_release, callback_returning, unregister_started;
static PSN_GUEST_ABI void blocking_callback(int32_t user, int32_t state, void *argument) {
    (void)user;
    (void)state;
    (void)argument;
    pthread_mutex_lock(&callback_test_mutex);
    callback_entered = 1;
    pthread_cond_broadcast(&callback_test_condition);
    while (!callback_release)
        pthread_cond_wait(&callback_test_condition, &callback_test_mutex);
    callback_returning = 1;
    pthread_mutex_unlock(&callback_test_mutex);
}
static void *dispatch_thread(void *arg) {
    (void)arg;
    assert(CALL0("sceNpCheckCallback") == 0);
    return NULL;
}
static void *unregister_thread(void *arg) {
    int32_t id = *(int32_t *)arg;
    pthread_mutex_lock(&callback_test_mutex);
    unregister_started = 1;
    pthread_cond_broadcast(&callback_test_condition);
    pthread_mutex_unlock(&callback_test_mutex);
    assert(CALLID("sceNpUnregisterStateCallbackA", id) == 0);
    pthread_mutex_lock(&callback_test_mutex);
    assert(callback_returning);
    pthread_mutex_unlock(&callback_test_mutex);
    return NULL;
}
static void test_concurrent_unregister(void) {
    int32_t id = ((RegisterFn)resolve("sceNpRegisterStateCallbackA"))(blocking_callback, NULL);
    pthread_t dispatcher, unregister;
    assert(id > 0 && !pthread_create(&dispatcher, NULL, dispatch_thread, NULL));
    pthread_mutex_lock(&callback_test_mutex);
    while (!callback_entered)
        pthread_cond_wait(&callback_test_condition, &callback_test_mutex);
    pthread_mutex_unlock(&callback_test_mutex);
    assert(!pthread_create(&unregister, NULL, unregister_thread, &id));
    pthread_mutex_lock(&callback_test_mutex);
    while (!unregister_started)
        pthread_cond_wait(&callback_test_condition, &callback_test_mutex);
    callback_release = 1;
    pthread_cond_broadcast(&callback_test_condition);
    pthread_mutex_unlock(&callback_test_mutex);
    assert(!pthread_join(dispatcher, NULL));
    assert(!pthread_join(unregister, NULL));
    int before = modern_calls;
    int32_t replacement =
        ((RegisterFn)resolve("sceNpRegisterStateCallbackA"))(modern_callback, &modern_calls);
    assert(replacement > 0 && replacement != id);
    assert(CALLID("sceNpUnregisterStateCallbackA", id) == (int32_t)0x80550009);
    assert(CALL0("sceNpCheckCallback") == 0 && modern_calls == before + 1);
    assert(CALLID("sceNpUnregisterStateCallbackA", replacement) == 0);
}

static void test_policy_and_identity(void) {
    unsigned char online[22];
    memset(online, 0xaa, sizeof(online));
    assert(((UserOutFn)resolve("sceNpGetOnlineId"))(test_user, online + 1) == 0);
    assert(online[0] == 0xaa && online[21] == 0xaa && online[17] == 0);
    assert(!memcmp(online + 1, "abcdefghijklmnop", 16));
    uint64_t account = 0;
    assert(((UserOutFn)resolve("sceNpGetAccountIdA"))(test_user, &account) == 0 && account == 0x123456789);
    assert(((UserOutFn)resolve("sceNpGetAccountIdA"))(2, &account) == (int32_t)0x80550007);
    typedef int32_t(PSN_GUEST_ABI *AccountUserFn)(uint64_t, int32_t *);
    int32_t local_user = -1;
    assert(((AccountUserFn)resolve("sceNpGetUserIdByAccountId"))(account, &local_user) == 0);
    assert(local_user == test_user);
    int32_t state = 99;
    identity_state = 0;
    assert(((UserOutFn)resolve("sceNpGetState"))(test_user, &state) == PSN_BACKEND_UNAVAILABLE &&
           state == 99);
    PsnEvent event = {0};
    event.kind = 1;
    event.user_id = test_user;
    assert(psn_host_identity_event(&event) == PSN_INVALID_ARGUMENT);
    identity_state = 2;
    Async async = {sizeof(async), 0, 0, 0};
    int32_t request = CALLPTR("sceNpCreateAsyncRequest", &async);
    struct {
        uint64_t size;
        int32_t user;
        uint32_t pad;
        uint64_t features;
        uint8_t reserved[32];
    } plus = {56, test_user, 0, 3, {0}};
    uint8_t result[35];
    memset(result, 0xaa, sizeof(result));
    typedef int32_t(PSN_GUEST_ABI * PlusFn)(int32_t, const void *, void *);
    assert(((PlusFn)resolve("sceNpCheckPlus"))(request, &plus, result + 1) == 0);
    assert(checked_features == 3 && result[0] == 0xaa && result[1] == 0 && result[34] == 0xaa);
    int32_t operation = 5;
    assert(POLL("sceNpPollAsync", request, &operation) == 0 && operation == 0);
    assert(CALLID("sceNpDeleteRequest", request) == 0);
    request = CALL0("sceNpCreateRequest");
    typedef int32_t(PSN_GUEST_ABI * ParentalFn)(int32_t, int32_t, int8_t *, void *);
    int8_t age = 0;
    uint8_t restrictions[5] = {7, 7, 7, 7, 7};
    assert(((ParentalFn)resolve("sceNpGetParentalControlInfoA"))(request, test_user, &age,
                                                                 restrictions + 1) == 0);
    assert(age == 13 && restrictions[0] == 7 && restrictions[4] == 7);
    assert(restrictions[1] == 0 && restrictions[2] == 1 && restrictions[3] == 0);
    assert(CALLID("sceNpDeleteRequest", request) == 0);
    uint8_t secret[128] = {0};
    secret[127] = 23;
    typedef int32_t(PSN_GUEST_ABI * TitleFn)(const char *, const void *);
    assert(((TitleFn)resolve("sceNpSetNpTitleId"))("CUSA13795", secret) == 0 &&
           !strcmp(seen_title, "CUSA13795"));
    struct {
        char country[4];
        int8_t age;
        uint8_t pad[3];
    } country = {{'I', 'T', 0, 0}, 12, {0}};
    struct {
        uint64_t size;
        int8_t age;
        uint8_t pad[3];
        int32_t count;
        const void *countries;
    } restriction_param = {24, 7, {0}, 1, &country};
    assert(CALLPTR("sceNpSetContentRestriction", &restriction_param) == 0);
}

typedef struct {
    char data[16], term, pad[3];
} Online;
typedef void(PSN_GUEST_ABI *VoidRegisterFn)(void *, void *);
static int presence_legacy_calls, presence_a_calls, presence_keep_calls, plus_calls,
    reachability_calls;
static int32_t presence_self_handle;
static int32_t reachability_values[3];
static void register_presence_legacy(void *callback, void *argument) {
    ((VoidRegisterFn)resolve("sceNpRegisterGamePresenceCallback"))(callback, argument);
}
static PSN_GUEST_ABI void presence_legacy_callback(const Online *online, void *argument) {
    assert(!backend_locked && argument == &presence_legacy_calls);
    assert(!strcmp(online->data, "Friend") && online->term == 0);
    ++presence_legacy_calls;
    register_presence_legacy(NULL, NULL);
    assert(CALL0("sceNpCheckCallback") == 0);
    assert(!strcmp(online->data, "Friend"));
}
static PSN_GUEST_ABI void presence_a_callback(const Online *online, void *argument) {
    assert(!backend_locked && argument == &presence_a_calls && !strcmp(online->data, "Friend"));
    ++presence_a_calls;
    assert(CALLID("sceNpUnregisterGamePresenceCallbackA", presence_self_handle) == 0);
}
static PSN_GUEST_ABI void presence_keep_callback(const Online *online, void *argument) {
    assert(!backend_locked && argument == &presence_keep_calls && !strcmp(online->data, "Friend"));
    ++presence_keep_calls;
}
static PSN_GUEST_ABI void plus_callback(int32_t user, int32_t event, void *argument) {
    assert(!backend_locked && user == test_user && event == 7 && argument == &plus_calls);
    ++plus_calls;
    assert(CALL0("sceNpUnregisterPlusEventCallback") == 0);
}
static PSN_GUEST_ABI void reachability_callback(int32_t user, int32_t state, void *argument) {
    assert(!backend_locked && user == test_user && argument == &reachability_calls);
    assert(reachability_calls < 3);
    reachability_values[reachability_calls++] = state;
}
static void test_presence_plus_reachability(void) {
    register_presence_legacy(presence_legacy_callback, &presence_legacy_calls);
    presence_self_handle = ((RegisterFn)resolve("sceNpRegisterGamePresenceCallbackA"))(
        presence_a_callback, &presence_a_calls);
    int32_t keep = ((RegisterFn)resolve("sceNpRegisterGamePresenceCallbackA"))(
        presence_keep_callback, &presence_keep_calls);
    assert(presence_self_handle > 0 && keep > 0);
    assert(((RegisterFn)resolve("sceNpRegisterPlusEventCallback"))(plus_callback, &plus_calls) ==
           0);
    assert(((RegisterFn)resolve("sceNpRegisterPlusEventCallback"))(plus_callback, &plus_calls) ==
           (int32_t)0x80550008);
    assert(((RegisterFn)resolve("sceNpRegisterNpReachabilityStateCallback"))(
               reachability_callback, &reachability_calls) == 0);
    assert(CALL0("sceNpCheckCallback") == 0);
    assert(!presence_legacy_calls && !presence_a_calls && !presence_keep_calls && !plus_calls &&
           !reachability_calls);
    PsnEvent event = {.kind = 4, .user_id = test_user};
    strcpy(event.data.online_id, "Friend");
    assert(psn_host_identity_event(&event) == 0);
    assert(psn_host_identity_event(&event) == 0);
    memset(event.data.online_id, 'x', sizeof(event.data.online_id));
    assert(psn_host_identity_event(&event) == PSN_INVALID_ARGUMENT);
    event = (PsnEvent){.kind = 5, .user_id = test_user};
    event.data.plus_event = 7;
    assert(psn_host_identity_event(&event) == 0);
    assert(psn_host_identity_event(&event) == 0);
    event = (PsnEvent){.kind = 6, .user_id = test_user};
    for (int32_t state = 0; state <= 2; ++state) {
        event.data.reachability = state;
        assert(psn_host_identity_event(&event) == 0);
    }
    event.data.reachability = 3;
    assert(psn_host_identity_event(&event) == PSN_INVALID_ARGUMENT);
    event = (PsnEvent){.kind = 1, .user_id = test_user};
    event.data.identity.state = 2;
    assert(psn_host_identity_event(&event) ==
           0); /* State events cannot call another callback family. */
    assert(CALL0("sceNpCheckCallback") == 0);
    assert(presence_legacy_calls == 1 && presence_a_calls == 1 && presence_keep_calls == 2 &&
           plus_calls == 1);
    assert(reachability_calls == 3 && reachability_values[0] == 0 && reachability_values[1] == 1 &&
           reachability_values[2] == 2);
    assert(CALLID("sceNpUnregisterStateCallbackA", keep) == (int32_t)0x80550009);
    assert(CALLID("sceNpUnregisterGamePresenceCallbackA", keep) == 0);
    assert(CALL0("sceNpUnregisterPlusEventCallback") == (int32_t)0x80550009);
    assert(CALL0("sceNpUnregisterNpReachabilityStateCallback") == 0);

    /* Legacy replacement must invalidate the queued delivery to the old generation. */
    event = (PsnEvent){.kind = 4, .user_id = test_user};
    strcpy(event.data.online_id, "Friend");
    register_presence_legacy(presence_legacy_callback, &presence_legacy_calls);
    assert(psn_host_identity_event(&event) == 0);
    register_presence_legacy(presence_keep_callback, &presence_keep_calls);
    assert(CALL0("sceNpCheckCallback") == 0 && presence_legacy_calls == 1 &&
           presence_keep_calls == 2);
    assert(psn_host_identity_event(&event) == 0);
    assert(CALL0("sceNpCheckCallback") == 0 && presence_keep_calls == 3);
    register_presence_legacy(NULL, NULL);

    int32_t handles[16];
    for (int i = 0; i < 16; ++i) {
        handles[i] = ((RegisterFn)resolve("sceNpRegisterGamePresenceCallbackA"))(
            presence_keep_callback, &presence_keep_calls);
        assert(handles[i] > 0);
    }
    assert(((RegisterFn)resolve("sceNpRegisterGamePresenceCallbackA"))(
               presence_keep_callback, &presence_keep_calls) == (int32_t)0x8055001D);
    for (int i = 0; i < 16; ++i)
        assert(CALLID("sceNpUnregisterGamePresenceCallbackA", handles[i]) == 0);
}
typedef struct {
    Online online;
    uint8_t options[8], reserved[8];
} NpId;
static void test_identity_comparison(void) {
    typedef int32_t(PSN_GUEST_ABI * CompareFn)(const void *, const void *);
    CompareFn compare_online = (CompareFn)resolve("sceNpCmpOnlineId");
    CompareFn compare_id = (CompareFn)resolve("sceNpCmpNpId");
    NpId first = {0}, second = {0};
    strcpy(first.online.data, "Player");
    strcpy(second.online.data, "Player");
    second.online.data[10] = 'x'; /* Unused bytes after the terminator are not the name. */
    assert(compare_online(&first.online, &second.online) == 0);
    assert(compare_id(&first, &second) == 0);
    second.options[3] = 1;
    assert(compare_id(&first, &second) == (int32_t)0x80550609);
    second.options[3] = 0;
    second.reserved[7] = 1;
    assert(compare_id(&first, &second) == (int32_t)0x80550609);
    second.reserved[7] = 0;
    second.online.data[0] = 'p';
    assert(compare_id(&first, &second) == (int32_t)0x80550609);
    assert(compare_online(NULL, &first.online) == PSN_INVALID_ARGUMENT);
    assert(compare_id(&first, NULL) == PSN_INVALID_ARGUMENT);
}
typedef struct {
    uint32_t count, padding;
    struct {
        Online online;
        uint32_t padding;
        uint64_t account;
    } users[16];
} Users;
typedef struct {
    void *arg;
    int32_t error, result;
    Users *users;
    uint8_t reserved[32];
} DialogResult;
typedef struct {
    const char *message, *session;
    int32_t address_type;
    uint32_t padding;
    const uint64_t *users;
    uint32_t count, padding2;
    uint8_t reserved[24];
} DialogData;
typedef struct {
    uint64_t base_size;
    uint8_t base_reserved[36];
    uint32_t magic;
    uint32_t size;
    int32_t mode, user;
    uint32_t padding;
    void *arg;
    const DialogData *data;
    uint8_t reserved[64];
} DialogParam;
static void test_dialog(void) {
    assert(CALL0("sceInvitationDialogInitialize") == 0);
    assert(CALL0("sceInvitationDialogInitialize") == (int32_t)0x80b80004);
    uint64_t recipient = 43;
    DialogData data = {"Join", "opaque-session", 1, 0, &recipient, 1, 0, {0}};
    DialogParam param = {0};
    param.base_size = 48;
    param.size = sizeof(param);
    param.mode = 1;
    param.user = test_user;
    param.arg = &param;
    param.data = &data;
    assert(CALLPTR("sceInvitationDialogOpenA", &param) == 0);
    dialog_pending = 1;
    assert(CALL0("sceInvitationDialogGetStatus") == 2);
    Users users = {0};
    DialogResult result = {0};
    result.users = &users;
    assert(CALLPTR("sceInvitationDialogGetResultA", &result) == (int32_t)0x80b80005);
    invitation_result = (PsnInvitationResult){0};
    invitation_result.result = 1;
    dialog_pending = 0;
    assert(CALL0("sceInvitationDialogUpdateStatus") == 3);
    assert(CALLPTR("sceInvitationDialogGetResultA", &result) == 0);
    assert(result.arg == &param && result.result == 1 && users.count == 0);
    assert(CALLPTR("sceInvitationDialogOpenA", &param) == 0 && dialog_released == 1);
    invitation_result = (PsnInvitationResult){0};
    invitation_result.user_count = 1;
    invitation_result.users[0].account_id = 43;
    strcpy(invitation_result.users[0].online_id, "Recipient");
    assert(CALL0("sceInvitationDialogUpdateStatus") == 3);
    assert(CALLPTR("sceInvitationDialogGetResultA", &result) == 0);
    assert(result.result == 0 && users.count == 1 && users.users[0].account == 43);
    assert(!strcmp(users.users[0].online.data, "Recipient"));
    param.mode = 2;
    param.data = NULL;
    assert(CALLPTR("sceInvitationDialogOpenA", &param) == 0);
    dialog_pending = 1;
    assert(CALL0("sceInvitationDialogClose") == 0 && dialog_canceled == 1);
    assert(CALLPTR("sceInvitationDialogGetResultA", &result) == 0 && result.result == 1 &&
           !users.count);
    assert(CALL0("sceInvitationDialogTerminate") == 0);
    assert(CALL0("sceInvitationDialogGetStatus") == 0);
}

int main(int argc, char **argv) {
    if (argc > 1)
        test_user = (int32_t)strtol(argv[1], NULL, 10);
    test_auth();
    test_repeated_auth_get();
    test_callbacks();
    test_concurrent_unregister();
    test_policy_and_identity();
    test_identity_comparison();
    test_presence_plus_reachability();
    test_dialog();
    psn_host_identity_reset();
    puts("PSN identity/auth/dialog contracts passed (host-only fixtures)");
    return 0;
}
