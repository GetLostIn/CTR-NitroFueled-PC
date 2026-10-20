/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Dynamic module and event ownership tests. Each case runs in a fresh process because
 * backend selection is immutable. No game, renderer, platform SDK, or network is used.
 * Usage: test-psn-backend <absolute fixture library path> <case> */
#define _POSIX_C_SOURCE 200809L
#include "psn_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int failures;
static unsigned states, pushes, identity_resets, webapi_resets;
static const char *test_case;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

typedef struct FixtureStats {
    unsigned opens, closes, event_calls, identity_calls, configuration_matches;
} FixtureStats;
typedef const FixtureStats *(*FixtureStatsFn)(void);

static void set_environment(const char *key, const char *value) {
#ifdef _WIN32
    CHECK(_putenv_s(key, value ? value : "") == 0);
#else
    CHECK((value ? setenv(key, value, 1) : unsetenv(key)) == 0);
#endif
}

static void overwrite_backend_buffers(void) {
    void *instance;
    const PsnBackendV1 *backend = psn_host_lock(&instance);
    CHECK(backend != NULL && instance != NULL);
    if (backend) {
        PsnIdentity identity;
        CHECK(backend->identity(instance, 1, &identity) == 0);
        CHECK(identity.account_id == UINT64_C(0xfedcba9876543210));
    }
    psn_host_unlock();
}

/* Stub the two guest adapters, not the loader. Reenter the backend during delivery to
 * invalidate its borrowed output and prove the core unlocked and copied before dispatch. */
int32_t psn_host_identity_event(const PsnEvent *event) {
    ++states;
    overwrite_backend_buffers();
    CHECK(event->kind == 1 && event->user_id == 1);
    CHECK(event->data.identity.state == 2);
    CHECK(event->data.identity.account_id == UINT64_C(0x123456789abcdef0));
    CHECK(strcmp(event->data.identity.online_id, "ChangedUser") == 0);
    return strcmp(test_case, "state-error") == 0 ? PSN_BACKEND_BUSY : 0;
}
int32_t psn_host_webapi_event(const PsnPushEvent *event) {
    ++pushes;
    overwrite_backend_buffers();
    CHECK(event->subscription == UINT64_C(0xf000000000000007));
    CHECK(event->from_account_id == UINT64_C(0x123456789abcdef0));
    CHECK(event->to_account_id == UINT64_C(0xfedcba9876543210));
    CHECK(strcmp(event->from_online_id, "Sender") == 0 &&
          strcmp(event->to_online_id, "Receiver") == 0);
    CHECK(event->from_platform == 3 && event->to_platform == 3 && event->service_label == 4);
    CHECK(strcmp(event->service, "") == 0);
    CHECK(strcmp(event->data_type, "np:service:presence:onlineStatus") == 0);
    CHECK(event->data.size == 5 && memcmp(event->data.data, "b\0in\xff", 5) == 0);
    CHECK(event->ext_data_count == 1);
    if (event->ext_data_count == 1) {
        CHECK(strcmp(event->ext_data[0].key, "additionalTrigger") == 0);
        CHECK(event->ext_data[0].value.size == 3 &&
              memcmp(event->ext_data[0].value.data, "e\0x", 3) == 0);
    }
    return strcmp(test_case, "push-error") == 0 ? PSN_BACKEND_BUSY : 0;
}
void psn_host_identity_dispatch(void) {}
void psn_host_webapi_dispatch(void) {}
void psn_host_identity_reset(void) { ++identity_resets; }
void psn_host_webapi_reset(void) { ++webapi_resets; }
static PSN_GUEST_ABI int32_t known_identity(void) { return 11; }
static PSN_GUEST_ABI int32_t known_webapi(void) { return 22; }
uintptr_t psn_host_identity_resolve(const char *name) {
    return strcmp(name, "sceNpKnownIdentity") == 0 ? (uintptr_t)known_identity : 0;
}
uintptr_t psn_host_webapi_resolve(const char *name) {
    return strcmp(name, "sceNpKnownWebApi") == 0 ? (uintptr_t)known_webapi : 0;
}
static void *retain_fixture(const char *path, FixtureStatsFn *stats) {
#ifdef _WIN32
    wchar_t wide[32768];
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768) != 0);
    HMODULE library = LoadLibraryExW(
        wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    FARPROC fn = library ? GetProcAddress(library, "psn_fixture_stats") : NULL;
    memcpy(stats, &fn, sizeof(*stats));
#else
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    void *fn = library ? dlsym(library, "psn_fixture_stats") : NULL;
    memcpy(stats, &fn, sizeof(*stats));
#endif
    CHECK(library != NULL && *stats != NULL);
    return library;
}
static void release_fixture(void *library) {
    if (!library)
        return;
#ifdef _WIN32
    FreeLibrary((HMODULE)library);
#else
    dlclose(library);
#endif
}

static void check_invitation_queue(void) {
    unsigned char actual[184], expected[180] = {0};
    const int32_t kind = 0x10000002;
    const int32_t user = strcmp(test_case, "good-arbitrary-user") == 0 ? 70001 : 3;
    const uint32_t flag = 0xa1b2c3d4u;
    const uint64_t account = UINT64_C(0xfedcba9876543210);
    CHECK(psn_host_system_count() == 1);
    CHECK(states == 1 && pushes == 1);
    /* Polling status does not expire/reoffer an accepted invitation or manufacture one
     * from friend/state pushes. Only receive consumes it. */
    for (unsigned i = 0; i < 20; ++i)
        CHECK(psn_host_system_count() == 1);
    CHECK(psn_host_system_receive(NULL) == (int32_t)0x80a10003);
    CHECK(psn_host_system_count() == 1);
    memset(actual, 0xcc, sizeof(actual));
    CHECK(psn_host_system_receive(actual) == 0);
    memcpy(expected, &kind, 4);
    memset(expected + 4, 'S', 45);
    expected[4] = 'A';
    memset(expected + 4 + 48, 'I', 60);
    memcpy(expected + 4 + 112, &flag, 4);
    memset(expected + 4 + 120, 'A', 16);
    memcpy(expected + 4 + 140, &user, 4);
    memset(expected + 4 + 144, 'R', 16);
    memcpy(expected + 4 + 168, &account, 8);
    CHECK(memcmp(actual, expected, sizeof(expected)) == 0);
    for (unsigned i = sizeof(expected); i < sizeof(actual); ++i)
        CHECK(actual[i] == 0xcc);
    CHECK(psn_host_system_count() == 0);
    memset(actual, 0xcc, sizeof(actual));
    for (unsigned i = 0; i < 20; ++i)
        CHECK(psn_host_system_receive(actual) == (int32_t)0x80a10004);
    for (unsigned i = 0; i < sizeof(actual); ++i)
        CHECK(actual[i] == 0xcc);
    CHECK(states == 1 && pushes == 1);
}

static void check_resolver(void) {
    CHECK(psn_host_resolve("sceNpKnownIdentity") == (uintptr_t)known_identity);
    CHECK(psn_host_resolve("sceNpKnownWebApi") == (uintptr_t)known_webapi);
    CHECK(psn_host_resolve("sceUnrelated") == 0);
    CHECK(psn_host_resolve("sceNpTrophyUnlockTrophy") == 0);
    typedef int32_t(PSN_GUEST_ABI * GuestFn)(void);
    GuestFn unsupported = (GuestFn)psn_host_resolve("sceNpUnsupported");
    CHECK(unsupported != NULL);
    if (unsupported)
        CHECK(unsupported() == PSN_BACKEND_UNSUPPORTED);
    CHECK(psn_host_resolve("sceInvitationDialogUnsupported") != 0);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <absolute fixture library> <case>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    test_case = argv[2];
    set_environment("PSN_TEST_CASE", test_case);
    set_environment("BB_PSN_BACKEND_CONFIG", "fixture configuration: = preserved");
    if (strcmp(test_case, "default") == 0 || strcmp(test_case, "empty") == 0) {
        set_environment("BB_PSN_BACKEND", strcmp(test_case, "empty") == 0 ? "" : NULL);
        CHECK(psn_host_selected() == 0 && psn_host_start() == 0);
        CHECK(psn_host_resolve("sceNpKnownIdentity") == 0);
        CHECK(psn_host_pump() == 0);
        set_environment("BB_PSN_BACKEND", path);
        CHECK(psn_host_selected() == 0 && psn_host_start() == 0);
        psn_host_stop();
        psn_host_stop();
        return failures ? 1 : 0;
    }
    if (strcmp(test_case, "unconfigured-users") == 0) {
        set_environment("BB_PSN_BACKEND", path);
        CHECK(psn_host_start() == PSN_INVALID_ARGUMENT);
        CHECK(psn_host_pump() == PSN_INVALID_ARGUMENT);
        psn_host_stop();
        return failures ? 1 : 0;
    }
    if (strcmp(test_case, "relative") == 0)
        path = "psn_backend_fixture.dll";
    char missing[32768];
    if (strcmp(test_case, "missing") == 0) {
        snprintf(missing, sizeof(missing), "%s.missing", path);
        path = missing;
    }
    int32_t users[] = {1, 3, 0, 70001};
    CHECK(psn_host_set_local_users(NULL, 1) == PSN_INVALID_ARGUMENT);
    CHECK(psn_host_set_local_users(users, 0) == PSN_INVALID_ARGUMENT);
    CHECK(psn_host_set_local_users(users, 5) == PSN_INVALID_ARGUMENT);
    int32_t duplicate[] = {1, 1}, invalid[] = {-1};
    CHECK(psn_host_set_local_users(duplicate, 2) == PSN_INVALID_ARGUMENT);
    CHECK(psn_host_set_local_users(invalid, 1) == PSN_INVALID_ARGUMENT);
    CHECK(psn_host_set_local_users(users, 4) == 0);
    users[3] = 7; /* The host owns a copy, not this caller's array. */
    int32_t copied[PSN_MAX_LOCAL_USERS];
    CHECK(psn_host_local_users(copied) == 4 && copied[3] == 70001);
    CHECK(psn_host_has_local_user(0) && psn_host_has_local_user(70001));
    CHECK(!psn_host_has_local_user(2) && !psn_host_has_local_user(-1));
    set_environment("BB_PSN_BACKEND", path);
    CHECK(psn_host_selected() == 1);
    const int init_failure =
        strcmp(test_case, "relative") == 0 || strcmp(test_case, "missing") == 0 ||
        strncmp(test_case, "bad-", 4) == 0 || strcmp(test_case, "missing-function") == 0 ||
        strcmp(test_case, "query-null") == 0 || strncmp(test_case, "open-", 5) == 0;
    int32_t result = psn_host_start();
    CHECK(psn_host_set_local_users(users, 4) == PSN_BACKEND_BUSY);
    CHECK(psn_host_set_local_users(copied, 4) == 0);
    if (init_failure) {
        int32_t expected = strcmp(test_case, "open-error") == 0     ? PSN_BACKEND_BUSY
                           : strcmp(test_case, "open-partial") == 0 ? PSN_BACKEND_ABORTED
                                                                    : PSN_BACKEND_UNAVAILABLE;
        CHECK(result == expected);
        set_environment("BB_PSN_BACKEND", NULL);
        CHECK(psn_host_selected() == 1 && psn_host_start() == expected);
        void *instance = (void *)1;
        CHECK(psn_host_lock(&instance) == NULL && instance == NULL);
        psn_host_unlock();
        CHECK(psn_host_pump() == expected);
        psn_host_stop();
        psn_host_stop();
        return failures ? 1 : 0;
    }
    CHECK(result == 0);
    if (result)
        return 1;
    FixtureStatsFn fixture_stats = NULL;
    void *library = retain_fixture(path, &fixture_stats);
    if (!fixture_stats) {
        psn_host_stop();
        release_fixture(library);
        return 1;
    }
    CHECK(fixture_stats()->opens == 1 && fixture_stats()->configuration_matches == 1);
    CHECK(psn_host_start() == 0 && fixture_stats()->opens == 1);
    check_resolver();
    if (strcmp(test_case, "good") == 0 || strcmp(test_case, "good-arbitrary-user") == 0) {
        check_invitation_queue();
        CHECK(fixture_stats()->identity_calls == 2);
    } else {
        int32_t expected = strcmp(test_case, "overflow") == 0 ||
                                   strcmp(test_case, "state-error") == 0 ||
                                   strcmp(test_case, "push-error") == 0
                               ? PSN_BACKEND_BUSY
                           : strcmp(test_case, "invalid-event") == 0 ? PSN_BACKEND_UNSUPPORTED
                           : strcmp(test_case, "event-error") == 0   ? PSN_BACKEND_ABORTED
                                                                     : PSN_INVALID_ARGUMENT;
        CHECK(psn_host_pump() == expected);
        const unsigned calls = fixture_stats()->event_calls;
        if (strcmp(test_case, "overflow") == 0)
            CHECK(calls == 33);
        CHECK(psn_host_pump() == expected);
        CHECK(psn_host_system_count() == expected);
        unsigned char event[180];
        CHECK(psn_host_system_receive(event) == expected);
        CHECK(fixture_stats()->event_calls == calls); /* sticky failure, no replay/fallback */
    }
    psn_host_stop();
    CHECK(fixture_stats()->closes == 1);
    CHECK(identity_resets == 1 && webapi_resets == 1);
    psn_host_stop();
    CHECK(fixture_stats()->closes == 1);
    release_fixture(library);
    printf("PSN backend %s: %s\n", test_case, failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
