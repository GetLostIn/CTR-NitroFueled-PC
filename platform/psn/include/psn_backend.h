/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PSN_BACKEND_H
#define PSN_BACKEND_H

/* Host binding for PS4 NP contracts, not a Sony SDK header or a wire protocol.
 * Native C calling convention, fixed-width fields, one selected backend per process.
 * No guest addresses, callbacks, host sockets or host-private structures cross this ABI.
 * Input strings/spans are borrowed for the call only: asynchronous implementations MUST
 * copy them before returning. Output spans are backend-owned until the next backend call;
 * the host copies them under its backend lock. No backend function calls into the host.
 * All calls are serialized by the host. Starts, polls and event_next must not wait for
 * network I/O; release/close quiesce workers without requiring another host call.
 * Successful start calls return a nonzero opaque handle. close() frees all resources.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PSN_BACKEND_ABI_V1 1u
#define PSN_CAP_IDENTITY (UINT64_C(1) << 0)
#define PSN_CAP_AUTH (UINT64_C(1) << 1)
#define PSN_CAP_WEBAPI (UINT64_C(1) << 2)
#define PSN_CAP_PUSH (UINT64_C(1) << 3)
#define PSN_CAP_INVITATION (UINT64_C(1) << 4)
#define PSN_PROFILE_SOCIAL_V1                                                                      \
    (PSN_CAP_IDENTITY | PSN_CAP_AUTH | PSN_CAP_WEBAPI | PSN_CAP_PUSH | PSN_CAP_INVITATION)
#define PSN_MAX_BODY (16u * 1024u * 1024u)
#define PSN_MAX_HEADERS 64u
#define PSN_MAX_EXT_DATA 32u
#define PSN_INVALID_ARGUMENT ((int32_t)0x80550003)
#define PSN_SIGNED_OUT ((int32_t)0x80550006)
/* Host binding errors, not claims about unverified Sony error constants. */
#define PSN_BACKEND_UNAVAILABLE ((int32_t)0x80020026)
#define PSN_BACKEND_UNSUPPORTED ((int32_t)0x8002004e)
#define PSN_BACKEND_BUSY ((int32_t)0x80020010)
#define PSN_BACKEND_ABORTED ((int32_t)0x80020055)
#define PSN_BACKEND_NO_MEMORY ((int32_t)0x8002000c)

typedef struct PsnBytes {
    const void *data;
    uint64_t size;
} PsnBytes;
typedef struct PsnHeader {
    const char *name;
    const char *value;
} PsnHeader;
typedef struct PsnIdentity {
    uint64_t account_id;
    char online_id[17];
    uint8_t reserved[3];
    int32_t state; /* 1 signed out, 2 signed in */
} PsnIdentity;
typedef struct PsnPolicy {
    int32_t availability; /* operation result, zero when available */
    uint8_t plus_authorized, age, content_restricted, chat_restricted, ugc_restricted;
    uint8_t reserved[3];
} PsnPolicy;
typedef struct PsnAuthTimeout {
    int32_t resolve_retry;
    uint32_t resolve_timeout, connect_timeout, send_timeout, receive_timeout;
} PsnAuthTimeout;
typedef struct PsnAgeRestriction {
    char country[4];
    int32_t age;
} PsnAgeRestriction;
typedef struct PsnContentRestriction {
    int32_t default_age;
    uint32_t count;
    const PsnAgeRestriction *countries;
} PsnContentRestriction;
typedef struct PsnAuthRequest {
    int32_t user_id;
    uint32_t kind;        /* 1 ID token, 2 authorization code */
    uint32_t api_variant; /* 0 legacy, 1 A, 3 V3 */
    uint32_t has_timeout;
    PsnAuthTimeout timeout;
    const char *client_id;
    const char *client_secret;
    const char *scope;
} PsnAuthRequest;
typedef struct PsnAuthResult {
    int32_t result, issuer_id;
    char value[4097]; /* token <=4096, authorization code <=128, terminated */
} PsnAuthResult;
typedef struct PsnWebRequest {
    int32_t user_id;
    uint32_t method; /* PS4 order: GET=0 POST=1 PUT=2 DELETE=3 PATCH=4 */
    const char *api_group;
    const char *path; /* includes the unmodified query string */
    const char *content_type;
    const PsnHeader *headers;
    uint32_t header_count;
    uint32_t reserved;
    PsnBytes body; /* binary/multipart bytes, never assumed to be JSON */
} PsnWebRequest;
typedef struct PsnWebResponse {
    int32_t result, http_status;
    const PsnHeader *headers;
    uint32_t header_count;
    uint32_t reserved;
    PsnBytes body, error_object;
} PsnWebResponse;
typedef struct PsnPushFilter {
    int32_t user_id;
    uint32_t kind;       /* 1 basic, 2 service, 3 extended service */
    const char *service; /* NULL: unqualified/basic; "": named empty service, distinct */
    const char *data_type;
    const char *const *ext_keys;
    uint32_t ext_key_count;
    uint32_t service_label;
} PsnPushFilter;
typedef struct PsnExtData {
    const char *key;
    PsnBytes value;
} PsnExtData;
typedef struct PsnPushEvent {
    uint64_t subscription; /* cookie supplied to subscribe(), never a guest pointer */
    uint64_t from_account_id, to_account_id;
    char from_online_id[17], to_online_id[17];
    uint8_t padding[2];
    int32_t from_platform, to_platform; /* PS3=1, Vita=2, PS4=3 */
    const char *service;
    const char *data_type;
    PsnBytes data;
    const PsnExtData *ext_data;
    uint32_t ext_data_count;
    uint32_t service_label;
} PsnPushEvent;
typedef struct PsnSessionInvitation {
    char session_id[46], invitation_id[61];
    uint8_t reserved;
    uint32_t flag;
    int32_t accepting_user_id;
    char accepting_online_id[17], referring_online_id[17];
    uint8_t padding[2];
    uint64_t referring_account_id;
} PsnSessionInvitation;
typedef struct PsnEvent {
    uint32_t kind; /* 1 NP state, 2 WebAPI push, 3 accepted invitation,
                     4 game presence, 5 Plus event, 6 NP reachability */
    int32_t user_id;
    union {
        PsnIdentity identity;
        PsnPushEvent push;
        PsnSessionInvitation invitation;
        char online_id[17];
        int32_t plus_event;
        int32_t reachability;
    } data;
} PsnEvent;
typedef struct PsnInvitationUser {
    uint64_t account_id;
    char online_id[17];
    uint8_t reserved[7];
} PsnInvitationUser;
typedef struct PsnInvitationRequest {
    int32_t user_id;
    uint32_t mode; /* 1 send, 2 receive */
    const char *session_id;
    const char *message;
    const PsnInvitationUser *users;
    uint32_t user_count; /* <=16 */
    uint32_t max_selectable_users;
} PsnInvitationRequest;
typedef struct PsnInvitationResult {
    int32_t error_code, result; /* PS4 common result: OK=0, canceled=1 */
    uint32_t user_count;
    uint32_t reserved;
    PsnInvitationUser users[16]; /* only recipients whose send actually succeeded */
} PsnInvitationResult;

typedef struct PsnBackendV1 {
    uint32_t size, abi_version;
    uint64_t capabilities;
    /* open: configuration is opaque to the host; failure never selects the legacy backend. */
    int32_t (*open)(const char *configuration, void **instance);
    void (*close)(void *instance);
    int32_t (*identity)(void *, int32_t user_id, PsnIdentity *);
    int32_t (*policy)(void *, int32_t user_id, uint64_t plus_features, PsnPolicy *);
    int32_t (*set_title)(void *, const char *title_id, PsnBytes secret);
    int32_t (*set_content_restriction)(void *, const PsnContentRestriction *);
    int32_t (*auth_start)(void *, const PsnAuthRequest *, uint64_t *request);
    int32_t (*auth_poll)(void *, uint64_t request, PsnAuthResult *); /* 1 pending, 0 complete */
    void (*auth_cancel)(void *, uint64_t request);
    void (*auth_release)(void *, uint64_t request); /* quiesces request before returning */
    int32_t (*web_start)(void *, const PsnWebRequest *, uint64_t *request);
    int32_t (*web_poll)(void *, uint64_t request, PsnWebResponse *); /* 1 pending, 0 complete */
    void (*web_cancel)(void *, uint64_t request);
    void (*web_release)(void *, uint64_t request);
    int32_t (*subscribe)(void *, uint64_t cookie, const PsnPushFilter *);
    void (*unsubscribe)(void *, uint64_t cookie); /* no subsequent events for cookie */
    int32_t (*event_next)(void *, PsnEvent *);    /* 1 event, 0 empty, negative error */
    int32_t (*invitation_start)(void *, const PsnInvitationRequest *, uint64_t *dialog);
    int32_t (*invitation_poll)(void *, uint64_t dialog, PsnInvitationResult *); /* 1 pending */
    void (*invitation_cancel)(void *, uint64_t dialog);
    void (*invitation_release)(void *, uint64_t dialog);
} PsnBackendV1;

/* Export this symbol using the native C ABI. Return NULL for an unknown ABI version.
 * The returned immutable table lives until module unload. No initialization in entry(). */
typedef const PsnBackendV1 *(*PsnBackendEntry)(uint32_t abi_version);
#define PSN_BACKEND_ENTRY "psn_backend_query"
#ifdef __cplusplus
}
#endif
#endif
