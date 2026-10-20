# Optional PSN backend integration

The online branch already implements private-server networking and its local PSN
identity in `runtime_net.c`. This change preserves that behavior and adds an
explicit extension point for other platform implementations. It does not require
a particular matchmaking service, launcher, platform SDK or publisher server.

The compatibility work uses the same [PSN host module](../../platform/psn/README.md)
and `PsnBackendV1` header as OCTR. The broader direction is a reusable platform
boundary across bbport-derived hosts and other PS4 compatibility projects. This
is not a claim that shadPS4 already implements this binding or that every PSN API
is implemented. A backend can be built and tested separately from either port.

## Existing defaults

With `BB_PSN_BACKEND` absent or empty, import resolution, local users, callbacks
and SystemService keep their original behavior. Without `BB_ONLINE_SERVER`, the
host stays offline. With that setting, the existing private-server path retains
its routing, name-based identity, credentials and API responses.

Selecting a backend replaces only the covered PSN contracts and notifications.
It does not enable the network, select a publisher server or change socket
restrictions. `BB_ONLINE_SERVER`, `BB_ONLINE_HTTP_PORT`, `BB_ONLINE_ALLOW` and the
existing launcher settings still govern networking. The launcher already passes
the process environment to the runtime; there is no new launcher preference and
no backend configuration is persisted.

The host owns local controller-slot user IDs, trophies, saves, UI and rendering.
`runtime_psn.c` supplies its local user-ID namespace explicitly instead of putting
this port's IDs into the reusable module. The backend receives copied native
values, never guest pointers or callback addresses.

## Integration points

- `runtime_start` initializes an explicitly selected backend before imports bind.
  A missing or incompatible backend stops activation; it never silently changes
  platform identity or falls back to fabricated credentials.
- `runtime_resolve` checks the PSN module before the existing network/services
  tables. Unimplemented NP operations fail explicitly in selected mode. Local
  trophy contracts and the existing stateless WebAPI NpId parser stay with the
  host.
- SystemService exposes the module's copied accepted-invitation queue. NP and
  WebAPI callbacks run through the game's existing callback pump, not directly
  from backend threads.
- The native C ABI is versioned and checks the required social profile. Backend
  requests, cancellation, buffers, event ownership and shutdown are documented
  in the common header and module README.

An optional authenticated loopback backend is included as one implementation of
the binding. It is disabled in the normal build and brings no Steam or publisher
dependency into the host. Other backends can use the same C ABI without HTTP.

## Verification without a game

`bash build.sh --test` builds the port and runs the common contract suite plus
the native integration tests. The focused suite can also run without a renderer:

```sh
cmake -S tests/psn -B out/native-psn-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/native-psn-tests
ctest --test-dir out/native-psn-tests --output-on-failure
```

The integration tests link the real import resolver, network and SystemService
implementations. They cover unselected and empty selection, the private-server
default, explicit selection with and without a network route, unchanged local
user names, unsupported-call isolation, and accepted-invitation delivery with
64-bit referring account IDs. The backend is a deterministic fixture; no game,
platform SDK or external service is started. The existing private-server network
test also runs against local listeners, including TCP/UDP, HTTP routing and
address restrictions. Live friends/invitation behavior and
cross-host multiplayer remain separate validation work.
