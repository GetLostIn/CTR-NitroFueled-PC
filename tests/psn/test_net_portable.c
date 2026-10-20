/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The existing network test creates its loopback listener before runtime startup.
 * Initialize Winsock for that listener and adapt its POSIX environment helpers. */
#ifdef _WIN32
#include <winsock2.h>
#include <stdlib.h>
static int setenv(const char *key, const char *value, int overwrite) {
    return !overwrite && getenv(key) ? 0 : _putenv_s(key, value);
}
static int unsetenv(const char *key) { return _putenv_s(key, ""); }
#define main private_server_test_main
#endif
#include "../test_net.c"
#ifdef _WIN32
#undef main
int main(int argc, char **argv) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    int result = private_server_test_main(argc, argv);
    WSACleanup();
    return result;
}
#endif
