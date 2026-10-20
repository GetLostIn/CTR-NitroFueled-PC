/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional platform backend. The unchanged services table is the default implementation. */
#define _POSIX_C_SOURCE 200809L
#include "psn_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static pthread_mutex_t backend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pump_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t system_mutex = PTHREAD_MUTEX_INITIALIZER;
static const PsnBackendV1 *backend;
static void *instance;
#ifdef _WIN32
static HMODULE module;
#else
static void *module;
#endif
static int selection = -1, initialized;
static int32_t failure;
#define INVITE_CAPACITY 32
static PsnSessionInvitation invitations[INVITE_CAPACITY];
static unsigned invitation_head, invitation_count;
static int32_t local_users[PSN_MAX_LOCAL_USERS];
static uint32_t local_user_count;

int32_t psn_host_set_local_users(const int32_t *users, uint32_t count) {
    if (!users || !count || count > PSN_MAX_LOCAL_USERS)
        return PSN_INVALID_ARGUMENT;
    int32_t copied[PSN_MAX_LOCAL_USERS];
    memcpy(copied, users, count * sizeof(*users));
    for (uint32_t i = 0; i < count; ++i) {
        if (copied[i] < 0)
            return PSN_INVALID_ARGUMENT;
        for (uint32_t j = 0; j < i; ++j)
            if (copied[i] == copied[j])
                return PSN_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&backend_mutex);
    int32_t result = 0;
    if (initialized && (count != local_user_count ||
                        memcmp(local_users, copied, count * sizeof(*users))))
        result = PSN_BACKEND_BUSY;
    if (!result) {
        memcpy(local_users, copied, count * sizeof(*users));
        local_user_count = count;
    }
    pthread_mutex_unlock(&backend_mutex);
    return result;
}
uint32_t psn_host_local_users(int32_t users[PSN_MAX_LOCAL_USERS]) {
    if (!users)
        return 0;
    pthread_mutex_lock(&backend_mutex);
    uint32_t count = local_user_count;
    memcpy(users, local_users, count * sizeof(*users));
    pthread_mutex_unlock(&backend_mutex);
    return count;
}
int psn_host_has_local_user(int32_t user) {
    int32_t users[PSN_MAX_LOCAL_USERS];
    uint32_t count = psn_host_local_users(users);
    for (uint32_t i = 0; i < count; ++i)
        if (users[i] == user)
            return 1;
    return 0;
}

int psn_host_selected(void) {
    /* Selection is immutable from the first runtime entry, before NP initialization. */
    pthread_mutex_lock(&backend_mutex);
    if (selection < 0) {
        const char *path = getenv("BB_PSN_BACKEND");
        selection = path && *path;
    }
    const int selected = selection;
    pthread_mutex_unlock(&backend_mutex);
    return selected;
}

static int valid_table(const PsnBackendV1 *b) {
    return b && b->size >= sizeof(*b) && b->abi_version == PSN_BACKEND_ABI_V1 &&
           (b->capabilities & PSN_PROFILE_SOCIAL_V1) == PSN_PROFILE_SOCIAL_V1 && b->open &&
           b->close && b->identity && b->policy && b->set_title && b->set_content_restriction &&
           b->auth_start && b->auth_poll && b->auth_cancel && b->auth_release && b->web_start &&
           b->web_poll && b->web_cancel && b->web_release && b->subscribe && b->unsubscribe &&
           b->event_next && b->invitation_start && b->invitation_poll && b->invitation_cancel &&
           b->invitation_release;
}
static void unload(void) {
    if (!module)
        return;
#ifdef _WIN32
    FreeLibrary(module);
#else
    dlclose(module);
#endif
    module = NULL;
}
int psn_host_start(void) {
    if (!psn_host_selected())
        return 0;
    pthread_mutex_lock(&backend_mutex);
    if (initialized) {
        int32_t result = failure;
        pthread_mutex_unlock(&backend_mutex);
        return result;
    }
    initialized = 1;
    if (!local_user_count) {
        failure = PSN_INVALID_ARGUMENT;
        fputs("PSN backend: the host has not configured its local user IDs.\n", stderr);
        pthread_mutex_unlock(&backend_mutex);
        return failure;
    }
    const char *path = getenv("BB_PSN_BACKEND");
    PsnBackendEntry query = NULL;
#ifdef _WIN32
    /* Require an explicit absolute path; dependencies are searched beside the module and
     * in system directories, never the working directory or a broad process PATH. */
    if (path && strlen(path) > 3 &&
        ((path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
         (path[0] == '\\' && path[1] == '\\'))) {
        wchar_t wide[32768];
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768))
            module = LoadLibraryExW(
                wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
    if (module) {
        FARPROC symbol = GetProcAddress(module, PSN_BACKEND_ENTRY);
        memcpy(&query, &symbol, sizeof(query));
    }
#else
    if (path && path[0] == '/')
        module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (module) {
        void *symbol = dlsym(module, PSN_BACKEND_ENTRY);
        memcpy(&query, &symbol, sizeof(query));
    }
#endif
    const PsnBackendV1 *candidate = query ? query(PSN_BACKEND_ABI_V1) : NULL;
    if (!valid_table(candidate)) {
        fputs("PSN backend: cannot load the selected module, ABI or required social profile is "
              "unavailable.\n",
              stderr);
        failure = PSN_BACKEND_UNAVAILABLE;
        unload();
    } else {
        const char *config = getenv("BB_PSN_BACKEND_CONFIG");
        failure = candidate->open(config ? config : "", &instance);
        if (failure || !instance) {
            if (instance)
                candidate->close(instance);
            instance = NULL;
            failure = failure < 0 ? failure : PSN_BACKEND_UNAVAILABLE;
            fputs("PSN backend: selected module failed to initialize.\n", stderr);
            unload();
        } else {
            backend = candidate;
            atexit(psn_host_stop);
            puts("Runtime: external PSN backend selected (social profile v1)");
        }
    }
    int32_t result = failure;
    pthread_mutex_unlock(&backend_mutex);
    return result;
}
const PsnBackendV1 *psn_host_lock(void **out) {
    pthread_mutex_lock(&backend_mutex);
    *out = instance;
    return backend;
}
void psn_host_unlock(void) { pthread_mutex_unlock(&backend_mutex); }

void psn_host_stop(void) {
    /* Called after guest threads stop (process teardown), never from a callback. */
    pthread_mutex_lock(&pump_mutex);
    psn_host_webapi_reset();
    psn_host_identity_reset();
    pthread_mutex_lock(&backend_mutex);
    if (backend)
        backend->close(instance);
    backend = NULL;
    instance = NULL;
    unload();
    pthread_mutex_unlock(&backend_mutex);
    pthread_mutex_lock(&system_mutex);
    invitation_count = invitation_head = 0;
    pthread_mutex_unlock(&system_mutex);
    pthread_mutex_unlock(&pump_mutex);
}

typedef struct CopiedPush {
    PsnPushEvent event;
    PsnExtData ext[PSN_MAX_EXT_DATA];
    char service[256], type[65], keys[PSN_MAX_EXT_DATA][33];
    void *payload;
} CopiedPush;
static int copy_text(char *out, size_t size, const char *value) {
    if (!value)
        return 0;
    size_t n = strnlen(value, size);
    if (n == size)
        return 0;
    memcpy(out, value, n + 1);
    return 1;
}
static int32_t copy_push(CopiedPush *out, const PsnPushEvent *in) {
    memset(out, 0, sizeof(*out));
    if (in->data.size > PSN_MAX_BODY || (in->data.size && !in->data.data) ||
        in->ext_data_count > PSN_MAX_EXT_DATA || (in->ext_data_count && !in->ext_data) ||
        !memchr(in->from_online_id, 0, sizeof(in->from_online_id)) ||
        !memchr(in->to_online_id, 0, sizeof(in->to_online_id)) ||
        (in->service && !copy_text(out->service, sizeof(out->service), in->service)) ||
        !copy_text(out->type, sizeof(out->type), in->data_type))
        return PSN_INVALID_ARGUMENT;
    size_t total = (size_t)in->data.size;
    for (uint32_t i = 0; i < in->ext_data_count; ++i) {
        const PsnExtData *e = &in->ext_data[i];
        if (e->value.size > PSN_MAX_BODY - total || (e->value.size && !e->value.data) ||
            !copy_text(out->keys[i], sizeof(out->keys[i]), e->key))
            return PSN_INVALID_ARGUMENT;
        total += (size_t)e->value.size;
    }
    out->payload = malloc(total ? total : 1);
    if (!out->payload)
        return PSN_BACKEND_NO_MEMORY;
    unsigned char *p = out->payload;
    out->event = *in;
    out->event.service = in->service ? out->service : NULL;
    out->event.data_type = out->type;
    out->event.ext_data = out->ext;
    if (in->data.size)
        memcpy(p, in->data.data, (size_t)in->data.size);
    out->event.data.data = p;
    p += in->data.size;
    for (uint32_t i = 0; i < in->ext_data_count; ++i) {
        out->ext[i].key = out->keys[i];
        out->ext[i].value = (PsnBytes){p, in->ext_data[i].value.size};
        if (out->ext[i].value.size)
            memcpy(p, in->ext_data[i].value.data, (size_t)out->ext[i].value.size);
        p += out->ext[i].value.size;
    }
    return 0;
}
static int32_t queue_invitation(const PsnSessionInvitation *invite) {
    if (!invite->session_id[0] || !memchr(invite->session_id, 0, sizeof(invite->session_id)) ||
        !memchr(invite->invitation_id, 0, sizeof(invite->invitation_id)) ||
        !memchr(invite->accepting_online_id, 0, sizeof(invite->accepting_online_id)) ||
        !memchr(invite->referring_online_id, 0, sizeof(invite->referring_online_id)) ||
        !psn_host_has_local_user(invite->accepting_user_id))
        return PSN_INVALID_ARGUMENT;
    pthread_mutex_lock(&system_mutex);
    int32_t result = PSN_BACKEND_BUSY;
    if (invitation_count < INVITE_CAPACITY) {
        invitations[(invitation_head + invitation_count++) % INVITE_CAPACITY] = *invite;
        result = 0;
    }
    pthread_mutex_unlock(&system_mutex);
    return result;
}
int32_t psn_host_pump(void) {
    if (!psn_host_selected())
        return 0;
    if (pthread_mutex_trylock(&pump_mutex))
        return 0;
    int32_t result = 0;
    /* Bound work per guest pump, not session duration. Overflow/fault is explicit. */
    for (unsigned i = 0; i < 64; ++i) {
        PsnEvent event = {0};
        CopiedPush copy = {0};
        void *ctx;
        const PsnBackendV1 *b = psn_host_lock(&ctx);
        if (failure)
            result = failure;
        else
            result = b ? b->event_next(ctx, &event) : PSN_BACKEND_UNAVAILABLE;
        if (result == 1 && event.kind == 2) {
            result = copy_push(&copy, &event.data.push);
            if (!result)
                result = 1;
        }
        psn_host_unlock();
        if (result != 1) {
            free(copy.payload);
            break;
        }
        result = 0;
        switch (event.kind) {
        case 1:
            if ((event.data.identity.state != 1 && event.data.identity.state != 2) ||
                !memchr(event.data.identity.online_id, 0, sizeof(event.data.identity.online_id)))
                result = PSN_INVALID_ARGUMENT;
            else
                result = psn_host_identity_event(&event);
            break;
        case 2:
            result = psn_host_webapi_event(&copy.event);
            break;
        case 3:
            result = queue_invitation(&event.data.invitation);
            break;
        case 4:
        case 5:
        case 6:
            result = psn_host_identity_event(&event);
            break;
        default:
            result = PSN_BACKEND_UNSUPPORTED;
            break;
        }
        free(copy.payload);
        if (result)
            break;
    }
    if (result < 0) {
        pthread_mutex_lock(&backend_mutex);
        failure = result;
        pthread_mutex_unlock(&backend_mutex);
        fputs("PSN backend: event delivery failed; the selected session remains failed.\n", stderr);
    }
    pthread_mutex_unlock(&pump_mutex);
    return result;
}

int32_t psn_host_system_count(void) {
    int32_t result = psn_host_pump();
    if (result < 0)
        return result;
    pthread_mutex_lock(&system_mutex);
    int32_t count = (int32_t)invitation_count;
    pthread_mutex_unlock(&system_mutex);
    return count;
}
int32_t psn_host_system_receive(void *event) {
    if (!event)
        return (int32_t)0x80a10003;
    int32_t result = psn_host_pump();
    if (result < 0)
        return result;
    pthread_mutex_lock(&system_mutex);
    if (!invitation_count) {
        pthread_mutex_unlock(&system_mutex);
        return (int32_t)0x80a10004;
    }
    PsnSessionInvitation invite = invitations[invitation_head];
    invitation_head = (invitation_head + 1) % INVITE_CAPACITY;
    --invitation_count;
    pthread_mutex_unlock(&system_mutex);
    /* Guest event param is 176 bytes. Use memcpy: param starts at event+4 and contains u64. */
    unsigned char *e = event, *p = e + 4;
    memset(e, 0, 4 + 176);
    const int32_t kind = 0x10000002;
    memcpy(e, &kind, 4);
    memcpy(p, invite.session_id, strlen(invite.session_id));
    memcpy(p + 48, invite.invitation_id, strlen(invite.invitation_id));
    memcpy(p + 112, &invite.flag, 4);
    memcpy(p + 120, invite.accepting_online_id, strlen(invite.accepting_online_id));
    memcpy(p + 140, &invite.accepting_user_id, 4);
    memcpy(p + 144, invite.referring_online_id, strlen(invite.referring_online_id));
    memcpy(p + 168, &invite.referring_account_id, 8);
    return 0;
}
static PSN_GUEST_ABI int64_t unsupported(void) {
    fputs("PSN backend: this guest NP operation is outside the supported host profile.\n", stderr);
    return PSN_BACKEND_UNSUPPORTED;
}
uintptr_t psn_host_resolve(const char *name) {
    if (!psn_host_selected())
        return 0;
    /* Local trophy storage/UI remains the host's responsibility in every mode. */
    if (name && !strncmp(name, "sceNpTrophy", 11))
        return 0;
    /* The existing host's local JSON-to-NpId utility has no backend state. */
    if (name && !strcmp(name, "sceNpWebApiUtilityParseNpId"))
        return 0;
    uintptr_t fn = psn_host_identity_resolve(name);
    if (fn)
        return fn;
    fn = psn_host_webapi_resolve(name);
    if (fn)
        return fn;
    const char *symbol = name;
    if (symbol && (!strncmp(symbol, "sceNp", 5) || !strncmp(symbol, "sceInvitationDialog", 19)))
        return (uintptr_t)unsupported;
    return 0;
}
