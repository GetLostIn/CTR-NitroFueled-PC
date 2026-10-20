# PSN host module

A PS4 platform-service adapter that builds independently of its host, renderer,
any game dump and any platform SDK. The host connects startup, import resolution,
SystemService and its own launcher/network policy. The native backend ABI keeps
platform implementations separate from those host responsibilities.

The game's existing PS4 imports remain the interface. `include/psn_backend.h` is
a versioned **host binding**, not an official Sony API header or a new social
protocol. A backend implements identity, credentials and platform services; this
module owns guest ABI conversion, handles, buffers, callback registrations and
invitation-dialog state. The publisher server and peer transport stay separate.

## Default behavior and selection

With `BB_PSN_BACKEND` unset or empty, the host uses its existing service path.
The module does not initialize a platform SDK or change that path's responses.
An external backend is opt-in:

```powershell
$env:BB_PSN_BACKEND = 'C:\Providers\example-psn-backend.dll'
$env:BB_PSN_BACKEND_CONFIG = 'provider-specific configuration'
```

On Linux use an absolute `.so` path and the same environment variables. The
configuration string is opaque to the host. Export `psn_backend_query` with the
native C calling convention and return a `PsnBackendV1` table for version 1.
All capability bits in `PSN_PROFILE_SOCIAL_V1` and all table functions are required.
Invalid paths, ABI versions, missing functions or initialization failures stop
activation; they never fall back to the host's built-in backend. Selection is fixed for the process.
Only load libraries you trust: this is an in-process extension, not a sandbox.

The optional [bridge backend](backends/README.md) is a concrete implementation
for an independently supervised provider. It uses authenticated loopback HTTP
and does not load Steam or link provider code. The dynamically loaded test
module remains a fixture, separate from this implementation.

Backend selection owns identity and social services only. It must not implicitly
choose publisher routing, enable online mode, select a tunnel or change socket
address/port policy. Each host configures those separately and prevents its
legacy identity or invitation path from mixing with the selected backend.
Local users, trophies, saves and rendering remain owned by the host. See the
[host integration note](../../docs/sacredfire/psn-host-backend.md) for its defaults
and transport options.

## Implemented contracts

| Surface | Host behavior |
|---|---|
| Identity and NP policy | Account/online ID/state, title and content restrictions, availability, Plus feature query and parental controls |
| NPAuth | Legacy/A/V3 token and authorization-code parameters, copied inputs, sync/async handles, pending/completion, abort/delete, pre-submission timeouts |
| NP callbacks | Legacy, toolkit and A state registrations; GamePresence (legacy/A), Plus and reachability events delivered only by `sceNpCheckCallback` / `ForLib` |
| NPWebAPI | Library/context/request lifetime, group/method/path/query, duplicate headers, binary or precomposed multipart bodies, status/error object, response headers and incremental reads |
| Push events | Basic, service and extended-A filters, service labels and extra data; host-owned registration cookies and callback frames |
| Accepted invitations | A separate SystemService `SessionInvitation` queue with the accepting user and referring player preserved |
| Invitation dialog | SEND/RECV, open/running/finished/result/close/terminate; only acknowledged recipients appear in successful results |

The NPAuth polling profile follows the modern PS4 behavior used by CTR: an unused
asynchronous request remains pending without overwriting the caller's result.
This does not claim compatibility with every historical SDK variant.

Unsupported NP operations return an explicit error in external mode. In particular,
the older extended-push callback signature, notification callback variants, NP
NotifyPlusFeature, manager SetTimeout,
dedicated multipart builders and pool statistics are not implemented. The host's
existing local WebAPI NpId parsing utility remains available. Score, TUS/TSS,
Matching2 and signaling are outside this profile.
The built-in implementation retains its existing behavior. The bridge backend
implements a narrower subset than the host profile: authorization-code requests
and invitation dialog UI fail explicitly; overlay-accepted invitations are
delivered through SystemService. Neither the module nor the backend claims a
complete PSN implementation or verified end-to-end online replacement.

## Ownership and events

- Backend calls use the native ABI and are serialized. Guest calls/callbacks use
  x86-64 System V. No guest pointer or callback crosses the backend boundary.
- Inputs are borrowed for one call. Backends copy anything they retain. Returned
  spans are borrowed until the next backend call; the host copies them first.
- Start/poll/event calls do not wait for network I/O. Cancel/release/close quiesce
  work without requiring another host call. The host releases requests before
  closing the backend and unloading its library.
- Event pumps copy data and release backend/registry locks before invoking guest
  callbacks. Self-unregister, context deletion and reentrant pumping are supported;
  concurrent unregistration waits for an active callback to finish.
- A push invitation notification and an accepted SystemService invitation are
  different events. Backend implementations must publish the original session
  data so the game can perform its normal session read and membership operations.
- Queue overflow or malformed events produce an error instead of silent loss.
  There is no artificial session/menu-idle deadline. Request timeouts remain bounded.

Before `psn_host_start()`, call `psn_host_set_local_users(ids, count)` with the
host's possible local user IDs (one to four). The module copies this snapshot;
IDs are opaque nonnegative values, including zero, rather than a presumed
`1..4` range. Changing the snapshot after startup is rejected. Identity lookup,
legacy online-ID/account-ID lookup, callback seeding and accepted invitations
use this set. The backend reports sign-in state; the snapshot does not log users
in or replace the host's UserService. `src/runtime_psn.c` supplies its host's
existing controller-slot IDs; another host supplies its own IDs.

Call `psn_host_start()` before resolving guest imports. Pass resolved symbol names
to `psn_host_resolve()` before the host's normal services table; zero means leave
the import to the host. Connect SystemService count/receive to the module only in
selected mode. Stop guest threads before explicit `psn_host_stop()`; process exit
also runs cleanup. `src/runtime_psn.c` shows the small integration shim.

## Build and test without a game

```sh
cmake -S platform/psn -B out/psn-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/psn-tests
ctest --test-dir out/psn-tests --output-on-failure
```

Requires an x86-64 GCC/Clang C11 compiler, CMake, pthreads (winpthreads on Windows)
and the OS dynamic loader. `bash build.sh --test` runs these tests as well.
Tests use synthetic data and a dynamically loaded fixture: no game, Steam, PSN,
publisher server, GPU or network connection is started. See the
[implementation note](../../docs/sacredfire/psn-host-backend.md) for evidence and
the distinction between automated checks and outstanding runtime validation.
Enabling `PSN_BRIDGE_BACKEND` also tests the real adapter against a private
loopback HTTP fixture, including guest callbacks and accepted-invitation bytes.
