/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real import resolver, network and SystemService adapters without
 * a game, renderer or platform service. Other runtime families are inert here. */
#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "runtime_psn.h"
#include "platform/psn/psn_host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define INERT_FAMILY(name) \
    uintptr_t runtime_##name##_resolve(const char *symbol) { (void)symbol; return 0; } \
    void runtime_##name##_report(void) {}
INERT_FAMILY(mutex)
INERT_FAMILY(thread)
INERT_FAMILY(content)
INERT_FAMILY(time)
INERT_FAMILY(sema)
INERT_FAMILY(eventflag)
INERT_FAMILY(json2)
INERT_FAMILY(rwlock)
INERT_FAMILY(memory)
INERT_FAMILY(kernel)
INERT_FAMILY(ajm)
INERT_FAMILY(audio)
INERT_FAMILY(pad)
INERT_FAMILY(rtc)
INERT_FAMILY(savedata)
INERT_FAMILY(file)
uintptr_t bbgpu_resolve(const char *symbol) { (void)symbol; return 0; }
int bbgpu_text_input_begin(const char *initial, const char *prompt) {
    (void)initial; (void)prompt; return 0;
}
int bbgpu_text_input_poll(char *out, uint64_t size) { (void)out; (void)size; return 2; }
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size) {
    (void)address; (void)size;
}
int runtime_pad_user_present(int user) { return user == 3; }
int64_t runtime_file_open(const char *path, int flags, int mode) {
    (void)path; (void)flags; (void)mode; return -1;
}
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) {
    (void)fd; (void)buffer; (void)size; return -1;
}
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
int32_t *runtime_errno(void) { static int32_t value; return &value; }

static void environment(const char *key, const char *value) {
#ifdef _WIN32
    assert(!_putenv_s(key, value ? value : ""));
#else
    assert(!(value ? setenv(key, value, 1) : unsetenv(key)));
#endif
}

static const struct { const char *nid, *symbol; } names[] = {
#include "import_names.inc"
};
static const char *nid(const char *symbol) {
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (!strcmp(names[i].symbol, symbol)) return names[i].nid;
    fprintf(stderr, "Missing import name for test: %s\n", symbol);
    abort();
}
static uintptr_t resolve(const char *symbol) {
    uintptr_t found = runtime_resolve(nid(symbol), 0);
    assert(found);
    return found;
}
typedef int32_t (ABI *GetState)(int32_t, int32_t *);
typedef int32_t (ABI *GetOnlineId)(int32_t, void *);
typedef int32_t (ABI *GetNetState)(int32_t *);
typedef int32_t (ABI *GetUsers)(int32_t *);
typedef int32_t (ABI *GetUserName)(int32_t, char *, uint64_t);
typedef int32_t (ABI *GetStatus)(unsigned char *);
typedef int32_t (ABI *GetEvent)(void *);
typedef int32_t (ABI *VoidCall)(void);

int main(int argc, char **argv) {
    assert(argc == 3);
    const char *mode = argv[2];
    const int selected = !strncmp(mode, "selected", 8);
    const int missing = !strcmp(mode, "selected-missing");
    const int online = !strcmp(mode, "online") || !strcmp(mode, "selected");
    char missing_path[32768];
    assert(snprintf(missing_path, sizeof(missing_path), "%s.missing", argv[1]) < (int)sizeof(missing_path));
    environment("BB_PSN_BACKEND", missing ? missing_path :
                (selected ? argv[1] : (!strcmp(mode, "empty") ? "" : NULL)));
    environment("BB_PSN_BACKEND_CONFIG", "fixture configuration: = preserved");
    environment("PSN_TEST_CASE", "good");
    environment("BB_ONLINE_SERVER", online ? "127.0.0.1" : NULL);
    environment("BB_ONLINE_ID", "Legacy Test");
    environment("BB_ONLINE_ACCOUNT_ID", NULL);
    environment("BB_USER_NAME", "Local User");
    runtime_start(1);
    assert(!missing); /* An explicit selection failure must terminate with exit 21. */
    assert(runtime_psn_selected() == selected);

    /* Network selection stays independent from platform selection. No packets are sent. */
    int32_t state = -1;
    assert(((GetNetState)resolve("sceNetCtlGetState"))(&state) == 0);
    assert(state == (online ? 3 : 0));
    uintptr_t legacy_np = online ? runtime_net_resolve(nid("sceNpGetState"))
                                 : runtime_services_resolve(nid("sceNpGetState"));
    uintptr_t actual_np = resolve("sceNpGetState");
    assert(selected ? actual_np != legacy_np : actual_np == legacy_np);
    assert(((GetState)actual_np)(1, &state) == 0);
    assert(state == (selected || online ? 2 : 1));
    unsigned char online_id[20] = {0};
    int32_t result = ((GetOnlineId)resolve("sceNpGetOnlineId"))(1, online_id);
    if (selected || online) {
        assert(result == 0);
        assert(!strcmp((char *)online_id, selected ? "FixtureUser" : "Legacy_Test"));
    } else assert((uint32_t)result == 0x80550006u);

    /* The host keeps local users/names and utilities; its controller slots are not Steam IDs. */
    int32_t users[4];
    assert(((GetUsers)resolve("sceUserServiceGetLoginUserIdList"))(users) == 0);
    assert(users[0] == 1 && users[1] == 3 && users[2] == -1 && users[3] == -1);
    char local_name[32];
    assert(((GetUserName)resolve("sceUserServiceGetUserName"))(1, local_name, sizeof(local_name)) == 0);
    assert(!strcmp(local_name, "Local User"));
    assert(((GetUserName)resolve("sceUserServiceGetUserName"))(3, local_name, sizeof(local_name)) == 0);
    assert(!strcmp(local_name, "Player 3"));
    const char *utility = nid("sceNpWebApiUtilityParseNpId");
    assert(runtime_resolve(utility, 0) == (online ? runtime_net_resolve(utility)
                                                : runtime_services_resolve(utility)));
    /* Unsupported NP calls cannot fall through to the private server's success stubs. */
    result = ((VoidCall)resolve("sceNpNotifyPlusFeature"))();
    assert(result == (selected ? PSN_BACKEND_UNSUPPORTED : 0));

    unsigned char status[12], event[180];
    assert(((GetStatus)resolve("sceSystemServiceGetStatus"))(status) == 0);
    int32_t count;
    memcpy(&count, status, sizeof(count));
    assert(count == (selected ? 1 : 0));
    result = ((GetEvent)resolve("sceSystemServiceReceiveEvent"))(event);
    if (selected) {
        assert(result == 0);
        int32_t kind, user;
        uint64_t referring;
        memcpy(&kind, event, sizeof(kind));
        memcpy(&user, event + 4 + 140, sizeof(user));
        memcpy(&referring, event + 4 + 168, sizeof(referring));
        assert(kind == 0x10000002 && user == 3);
        assert(referring == UINT64_C(0xfedcba9876543210));
        assert(event[4] == 'A' && event[4 + 44] == 'S' && event[4 + 45] == 0);
        assert(((GetEvent)resolve("sceSystemServiceReceiveEvent"))(event) == (int32_t)0x80a10004);
    } else assert((uint32_t)result == 0x80a10004u);
    puts("native-psn-test: host defaults, routing, users and backend boundary passed");
    return 0;
}
