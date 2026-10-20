/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A deterministic shared module for host-boundary tests. No platform SDK or network. */
#include "../include/psn_backend.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef struct FixtureStats {
    unsigned opens, closes, event_calls, identity_calls, configuration_matches;
} FixtureStats;

static FixtureStats stats;
static unsigned event_index;
static char service[32], data_type[65], key[33];
static unsigned char body[5], extra[3];
static PsnExtData ext;

static int is_case(const char *name) {
    const char *mode = getenv("PSN_TEST_CASE");
    return mode && strcmp(mode, name) == 0;
}

EXPORT const FixtureStats *psn_fixture_stats(void) { return &stats; }

static void poison_borrowed_output(void) {
    memset(service, 'x', sizeof(service) - 1);
    service[sizeof(service) - 1] = 0;
    strcpy(data_type, "overwritten");
    strcpy(key, "overwritten");
    memset(body, 0xdd, sizeof(body));
    memset(extra, 0xee, sizeof(extra));
    ext.key = "overwritten";
    ext.value = (PsnBytes){NULL, 0};
}

static int32_t fixture_open(const char *configuration, void **instance) {
    ++stats.opens;
    stats.configuration_matches = strcmp(configuration, "fixture configuration: = preserved") == 0;
    if (!stats.configuration_matches)
        return PSN_INVALID_ARGUMENT;
    if (is_case("open-null")) {
        *instance = NULL;
        return 0;
    }
    if (is_case("open-error")) {
        *instance = NULL;
        return PSN_BACKEND_BUSY;
    }
    *instance = &stats;
    return is_case("open-partial") ? PSN_BACKEND_ABORTED : 0;
}

static void fixture_close(void *instance) {
    if (instance == &stats)
        ++stats.closes;
}

static int32_t fixture_identity(void *instance, int32_t user_id, PsnIdentity *out) {
    if (instance != &stats || !out || user_id != 1)
        return PSN_INVALID_ARGUMENT;
    ++stats.identity_calls;
    poison_borrowed_output();
    *out = (PsnIdentity){.account_id = UINT64_C(0xfedcba9876543210), .state = 2};
    strcpy(out->online_id, "FixtureUser");
    return 0;
}
static int32_t fixture_policy(void *ctx, int32_t user_id, uint64_t features, PsnPolicy *out) {
    (void)ctx;
    (void)user_id;
    (void)features;
    memset(out, 0, sizeof(*out));
    return 0;
}
static int32_t fixture_title(void *ctx, const char *id, PsnBytes secret) {
    (void)ctx;
    (void)id;
    (void)secret;
    return 0;
}
static int32_t fixture_restriction(void *ctx, const PsnContentRestriction *restriction) {
    (void)ctx;
    (void)restriction;
    return 0;
}
static int32_t fixture_auth_start(void *ctx, const PsnAuthRequest *request, uint64_t *id) {
    (void)ctx;
    (void)request;
    (void)id;
    return PSN_BACKEND_UNSUPPORTED;
}
static int32_t fixture_auth_poll(void *ctx, uint64_t id, PsnAuthResult *out) {
    (void)ctx;
    (void)id;
    (void)out;
    return PSN_BACKEND_UNSUPPORTED;
}
static int32_t fixture_web_start(void *ctx, const PsnWebRequest *request, uint64_t *id) {
    (void)ctx;
    (void)request;
    (void)id;
    return PSN_BACKEND_UNSUPPORTED;
}
static int32_t fixture_web_poll(void *ctx, uint64_t id, PsnWebResponse *out) {
    (void)ctx;
    (void)id;
    (void)out;
    return PSN_BACKEND_UNSUPPORTED;
}
static int32_t fixture_subscribe(void *ctx, uint64_t cookie, const PsnPushFilter *filter) {
    (void)ctx;
    (void)cookie;
    (void)filter;
    return 0;
}
static void fixture_release(void *ctx, uint64_t id) {
    (void)ctx;
    (void)id;
}
static int32_t fixture_invitation_start(void *ctx, const PsnInvitationRequest *request,
                                        uint64_t *id) {
    (void)ctx;
    (void)request;
    (void)id;
    return PSN_BACKEND_UNSUPPORTED;
}
static int32_t fixture_invitation_poll(void *ctx, uint64_t id, PsnInvitationResult *out) {
    (void)ctx;
    (void)id;
    (void)out;
    return PSN_BACKEND_UNSUPPORTED;
}

static void make_invitation(PsnEvent *event, unsigned index) {
    memset(event, 0, sizeof(*event));
    event->kind = 3;
    event->user_id = 3;
    PsnSessionInvitation *invite = &event->data.invitation;
    memset(invite->session_id, 'S', 45);
    invite->session_id[0] = (char)('A' + index % 26);
    memset(invite->invitation_id, 'I', 60);
    invite->flag = 0xa1b2c3d4u;
    invite->accepting_user_id = is_case("good-arbitrary-user") ? 70001 : 3;
    memset(invite->accepting_online_id, 'A', 16);
    memset(invite->referring_online_id, 'R', 16);
    invite->referring_account_id = UINT64_C(0xfedcba9876543210);
}

static int32_t fixture_event_next(void *ctx, PsnEvent *event) {
    if (ctx != &stats || !event)
        return PSN_INVALID_ARGUMENT;
    ++stats.event_calls;
    poison_borrowed_output();
    memset(event, 0, sizeof(*event));
    if (is_case("event-error"))
        return PSN_BACKEND_ABORTED;
    if (is_case("invalid-event")) {
        event->kind = 99;
        return 1;
    }
    if (is_case("overflow")) {
        make_invitation(event, event_index++);
        return 1;
    }
    if (event_index == 0) {
        make_invitation(event, event_index++);
        if (is_case("invalid-invite"))
            memset(event->data.invitation.session_id, 'X', 46);
        if (is_case("invalid-user"))
            event->data.invitation.accepting_user_id = 5;
        return 1;
    }
    if (event_index == 1) {
        ++event_index;
        event->kind = 1;
        event->user_id = 1;
        event->data.identity =
            (PsnIdentity){.account_id = UINT64_C(0x123456789abcdef0), .state = 2};
        strcpy(event->data.identity.online_id, "ChangedUser");
        if (is_case("invalid-state"))
            event->data.identity.state = 3;
        return 1;
    }
    if (event_index == 2) {
        ++event_index;
        event->kind = 2;
        event->user_id = 1;
        service[0] = 0; /* Empty service is a meaningful part of a service push filter. */
        strcpy(data_type, "np:service:presence:onlineStatus");
        strcpy(key, "additionalTrigger");
        memcpy(body, "b\0in\xff", 5);
        memcpy(extra, "e\0x", 3);
        ext = (PsnExtData){key, {extra, sizeof(extra)}};
        PsnPushEvent *push = &event->data.push;
        push->subscription = UINT64_C(0xf000000000000007);
        push->from_account_id = UINT64_C(0x123456789abcdef0);
        push->to_account_id = UINT64_C(0xfedcba9876543210);
        strcpy(push->from_online_id, "Sender");
        strcpy(push->to_online_id, "Receiver");
        push->from_platform = push->to_platform = 3;
        push->service = service;
        push->data_type = data_type;
        push->data = (PsnBytes){body, sizeof(body)};
        push->ext_data = &ext;
        push->ext_data_count = 1;
        push->service_label = 4;
        if (is_case("invalid-push"))
            push->data.data = NULL;
        if (is_case("oversized-push"))
            push->data.size = PSN_MAX_BODY + UINT64_C(1);
        return 1;
    }
    return 0;
}

EXPORT const PsnBackendV1 *psn_backend_query(uint32_t version) {
    static PsnBackendV1 table;
    if (version != PSN_BACKEND_ABI_V1 || is_case("query-null"))
        return NULL;
    table = (PsnBackendV1){.size = sizeof(table),
                           .abi_version = PSN_BACKEND_ABI_V1,
                           .capabilities = PSN_PROFILE_SOCIAL_V1,
                           .open = fixture_open,
                           .close = fixture_close,
                           .identity = fixture_identity,
                           .policy = fixture_policy,
                           .set_title = fixture_title,
                           .set_content_restriction = fixture_restriction,
                           .auth_start = fixture_auth_start,
                           .auth_poll = fixture_auth_poll,
                           .auth_cancel = fixture_release,
                           .auth_release = fixture_release,
                           .web_start = fixture_web_start,
                           .web_poll = fixture_web_poll,
                           .web_cancel = fixture_release,
                           .web_release = fixture_release,
                           .subscribe = fixture_subscribe,
                           .unsubscribe = fixture_release,
                           .event_next = fixture_event_next,
                           .invitation_start = fixture_invitation_start,
                           .invitation_poll = fixture_invitation_poll,
                           .invitation_cancel = fixture_release,
                           .invitation_release = fixture_release};
    if (is_case("bad-abi"))
        ++table.abi_version;
    if (is_case("bad-size"))
        --table.size;
    if (is_case("bad-capabilities"))
        table.capabilities &= ~PSN_CAP_PUSH;
    if (is_case("missing-function"))
        table.invitation_poll = NULL;
    return &table;
}
