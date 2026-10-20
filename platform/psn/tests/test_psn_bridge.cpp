/* SPDX-License-Identifier: GPL-2.0-or-later */
extern "C" {
#include "../psn_host.h"
}
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr uint64_t owner = UINT64_C(76561198000000001), sender = UINT64_C(76561198000000002);
static unsigned callbacks;
static int32_t local_user = 1;
static unsigned state_callbacks;
static void PSN_GUEST_ABI state_callback(int32_t user, int32_t state, void *) {
    assert(user == local_user && state == 2);
    ++state_callbacks;
}
static std::thread::id main_thread;
struct DataType {
    char value[65];
};
struct ExtKey {
    char value[33];
};
struct ExtFilter {
    DataType type;
    ExtKey *keys;
    uint64_t count;
};
struct ExtData {
    ExtKey key;
    const void *data;
    uint64_t size;
};
struct OnlineId {
    char name[16], term;
    unsigned char reserved[3];
};
struct PeerA {
    uint64_t account;
    int32_t platform;
    unsigned char reserved[4];
};
using Initialize = int32_t(PSN_GUEST_ABI *)(int32_t, uint64_t);
using One = int32_t(PSN_GUEST_ABI *)(int32_t);
using Two = int32_t(PSN_GUEST_ABI *)(int32_t, int32_t);
using Pump = int32_t(PSN_GUEST_ABI *)();
using Filter = int32_t(PSN_GUEST_ABI *)(int32_t, int32_t, const char *, uint32_t, const ExtFilter *,
                                        uint64_t);
using Register = int32_t(PSN_GUEST_ABI *)(int32_t, int32_t, void *, void *);
template <class T> T fn(const char *name) {
    auto ptr = psn_host_resolve(name);
    assert(ptr);
    return reinterpret_cast<T>(ptr);
}
static void PSN_GUEST_ABI callback(int32_t context, int32_t id, const char *service, uint32_t label,
                                   const PeerA *to, const OnlineId *to_name, const PeerA *from,
                                   const OnlineId *from_name, const DataType *type,
                                   const void *data, uint64_t size, const ExtData *ext,
                                   uint64_t count, void *) {
    assert(std::this_thread::get_id() == main_thread);
    assert(!service && label == UINT32_MAX && to && from && to->account == owner &&
           from->account == sender);
    assert(to_name && from_name && !strcmp(to_name->name, "Receiver") &&
           !strcmp(from_name->name, "Sender"));
    assert(!strcmp(type->value, "np:service:presence:onlineStatus"));
    assert(std::string(static_cast<const char *>(data), size) == "{\"onlineStatus\":\"online\"}");
    assert(count == 1 && !strcmp(ext[0].key.value, "fromAccountId"));
    assert(std::string(static_cast<const char *>(ext[0].data), ext[0].size) ==
           std::to_string(sender));
    ++callbacks;
    // Reentrant unregistration must suppress a second copied event for this
    // cookie.
    assert(fn<Two>("sceNpWebApiUnregisterExtdPushEventCallback")(context, id) == 0);
    assert(fn<Pump>("sceNpCheckCallback")() == 0);
}
static void environment(const char *key, const std::string &value) {
#ifdef _WIN32
    assert(!_putenv_s(key, value.c_str()));
#else
    assert(!setenv(key, value.c_str(), 1));
#endif
}
static void pause() { std::this_thread::sleep_for(5ms); }
static void trigger(const char *path, bool allow_session_failure = false) {
    void *instance;
    auto b = psn_host_lock(&instance);
    PsnWebRequest r{};
    r.user_id = local_user;
    r.method = 1;
    r.api_group = "test";
    r.path = path;
    r.content_type = "application/octet-stream";
    const unsigned char bytes[] = {0, 1, 255};
    r.body = {bytes, 3};
    uint64_t id = 0;
    assert(!b->web_start(instance, &r, &id));
    psn_host_unlock();
    for (unsigned n = 0; n < 2000; ++n) {
        b = psn_host_lock(&instance);
        PsnWebResponse response{};
        auto rc = b->web_poll(instance, id, &response);
        if (!rc) {
            if (allow_session_failure && response.result == PSN_BACKEND_UNAVAILABLE) {
                b->web_release(instance, id);
                psn_host_unlock();
                return;
            }
            assert(!response.result && response.http_status == 207 && response.body.size == 3);
            assert(!memcmp(response.body.data, bytes, 3));
            b->web_release(instance, id);
            psn_host_unlock();
            return;
        }
        assert(rc == 1);
        psn_host_unlock();
        pause();
    }
    assert(false);
}
int main(int argc, char **argv) {
    assert(argc == 5);
    main_thread = std::this_thread::get_id();
    std::string mode = argv[4];
    environment("BB_PSN_BACKEND", argv[1]);
    const bool success_mode = mode == "good" || mode == "arbitrary-user" || mode == "zero-user";
    local_user = mode == "arbitrary-user" ? 70001 : mode == "zero-user" ? 0 : 1;
    Json config{{"url", argv[2]}, {"token", argv[3]}};
    if (mode == "arbitrary-user" || mode == "zero-user")
        config["local_user_id"] = local_user;
    environment("BB_PSN_BACKEND_CONFIG", config.dump());
    assert(!psn_host_set_local_users(&local_user, 1));
    auto started = psn_host_start();
    if (started)
        std::fprintf(stderr, "backend startup status: 0x%08x\n", static_cast<unsigned>(started));
    if (mode == "bad-info" || mode == "bad-token" || mode == "redirect" || mode == "nonloopback") {
        assert(started < 0);
        return 0;
    }
    assert(!started);
    using StateRegister = int32_t(PSN_GUEST_ABI *)(void *, void *);
    auto state_handle = fn<StateRegister>("sceNpRegisterStateCallbackA")(
        reinterpret_cast<void *>(state_callback), nullptr);
    assert(state_handle > 0 && !fn<Pump>("sceNpCheckCallback")() && state_callbacks == 1);
    assert(!fn<One>("sceNpUnregisterStateCallbackA")(state_handle));
    void *instance;
    auto b = psn_host_lock(&instance);
    PsnIdentity identity{};
    assert(!b->identity(instance, local_user, &identity) && identity.account_id == owner &&
           identity.state == 2);
    assert(b->identity(instance, 2, &identity) == PSN_INVALID_ARGUMENT);
    PsnAuthRequest auth{};
    auth.user_id = local_user;
    auth.kind = 2;
    auth.client_id = "client";
    auth.scope = "scope";
    uint64_t request = 0;
    assert(b->auth_start(instance, &auth, &request) == PSN_BACKEND_UNSUPPORTED);
    auth.kind = 1;
    auth.client_secret = "synthetic-application-secret";
    assert(!b->auth_start(instance, &auth, &request));
    psn_host_unlock();
    bool completed = false;
    for (unsigned n = 0; n < 1200; ++n) {
        b = psn_host_lock(&instance);
        PsnAuthResult result{};
        auto rc = b->auth_poll(instance, request, &result);
        if (!rc) {
            assert(!result.result && result.issuer_id == 256 &&
                   !strcmp(result.value, "fixture-local-capability"));
            b->auth_release(instance, request);
            completed = true;
        }
        psn_host_unlock();
        if (completed)
            break;
        assert(rc == 1);
        pause();
    }
    assert(completed);
    // All inputs are copied; binary bodies and exact decimal IDs survive the
    // relay.
    trigger("/binary?keep=%2F");
    if (mode == "good")
        trigger("/slow-complete"); // A real six-second join-like reply is allowed.
    b = psn_host_lock(&instance);
    PsnHeader duplicate[] = {{"X-Test", "one"}, {"x-test", "two"}};
    PsnWebRequest invalid{};
    invalid.user_id = local_user;
    invalid.api_group = "test";
    invalid.path = "/binary";
    invalid.headers = duplicate;
    invalid.header_count = 2;
    assert(b->web_start(instance, &invalid, &request) == PSN_BACKEND_UNSUPPORTED);
    invalid.header_count = 0;
    invalid.body = {nullptr, 256 * 1024 + 1};
    assert(b->web_start(instance, &invalid, &request) == PSN_BACKEND_UNSUPPORTED);
    PsnInvitationRequest invite{};
    assert(b->invitation_start(instance, &invite, &request) == PSN_BACKEND_UNSUPPORTED);
    PsnWebRequest slow{};
    slow.user_id = local_user;
    slow.api_group = "test";
    slow.path = "/slow";
    assert(!b->web_start(instance, &slow, &request));
    psn_host_unlock();
    std::this_thread::sleep_for(100ms); // Exercise cancellation after the HTTP work has started.
    b = psn_host_lock(&instance);
    auto cancel_start = std::chrono::steady_clock::now();
    b->web_cancel(instance, request);
    PsnWebResponse aborted{};
    assert(!b->web_poll(instance, request, &aborted) && aborted.result == PSN_BACKEND_ABORTED);
    b->web_release(instance, request);
    psn_host_unlock();
    assert(std::chrono::steady_clock::now() - cancel_start < 2500ms);
    auto lib = fn<Initialize>("sceNpWebApiInitialize")(0, 1024 * 1024);
    assert(lib > 0);
    auto context = fn<Two>("sceNpWebApiCreateContextA")(lib, local_user);
    assert(context > 0);
    auto handle = fn<One>("sceNpWebApiCreateHandle")(lib);
    assert(handle > 0);
    ExtKey key{{"fromAccountId"}};
    ExtFilter filter{{"np:service:presence:onlineStatus"}, &key, 1};
    auto filter_id =
        fn<Filter>("sceNpWebApiCreateExtdPushEventFilter")(lib, handle, nullptr, 0, &filter, 1);
    assert(filter_id > 0);
    assert(fn<Register>("sceNpWebApiRegisterExtdPushEventCallbackA")(
               context, filter_id, reinterpret_cast<void *>(callback), nullptr) > 0);
    assert(fn<Register>("sceNpWebApiRegisterExtdPushEventCallbackA")(
               context, filter_id, reinterpret_cast<void *>(callback), nullptr) > 0);
    if (mode == "overflow") {
        b = psn_host_lock(&instance);
        PsnPushFilter many{};
        many.user_id = local_user;
        many.kind = 1;
        many.data_type = "np:service:presence:onlineStatus";
        for (uint64_t n = 10000; n < 10256; ++n)
            assert(!b->subscribe(instance, n, &many));
        psn_host_unlock();
    }
    trigger(success_mode ? "/trigger" : ("/" + mode).c_str(), !success_mode);
    if (!success_mode) {
        bool failed = false;
        for (unsigned n = 0; n < 1000; ++n) {
            if (fn<Pump>("sceNpCheckCallback")() < 0) {
                failed = true;
                break;
            }
            pause();
        }
        assert(failed && callbacks == 0);
        psn_host_stop();
        return 0;
    }
    // Receiving on the worker cannot execute callbacks, even while events are
    // queued.
    std::this_thread::sleep_for(250ms);
    assert(callbacks == 0);
    for (unsigned n = 0; n < 1000 && callbacks == 0; ++n) {
        assert(!fn<Pump>("sceNpCheckCallback")());
        pause();
    }
    assert(callbacks == 2);
    for (unsigned n = 0; n < 1000 && psn_host_system_count() == 0; ++n)
        pause();
    assert(psn_host_system_count() == 1);
    unsigned char e[180]{};
    assert(!psn_host_system_receive(e));
    uint32_t kind = 0;
    uint64_t account = 0;
    memcpy(&kind, e, 4);
    memcpy(&account, e + 4 + 168, 8);
    int32_t accepting_user = -1;
    memcpy(&accepting_user, e + 4 + 140, sizeof(accepting_user));
    assert(accepting_user == local_user);
    assert(kind == 0x10000002 && account == sender &&
           !strcmp(reinterpret_cast<char *>(e + 4), "76561198099999999"));
    assert(!strcmp(reinterpret_cast<char *>(e + 4 + 48), "76561198099999999-76561198000000002-1"));
    assert(!strcmp(reinterpret_cast<char *>(e + 4 + 144), "Sender"));
    std::this_thread::sleep_for(350ms);
    assert(psn_host_system_count() == 0 && callbacks == 2);
    assert(!fn<One>("sceNpWebApiTerminate")(lib));
    psn_host_stop();
    puts("Bridge backend: real HTTP, copied async results, cancellation, guest "
         "callback and accepted invite passed");
}
