/* Link-time stand-ins; the device's libcutils provides the real ones. */
#include <cutils/native_handle.h>
#include <stddef.h>

native_handle_t *native_handle_create(int numFds, int numInts) { (void)numFds; (void)numInts; return NULL; }
native_handle_t *native_handle_clone(const native_handle_t *handle) { (void)handle; return NULL; }
int native_handle_close(const native_handle_t *h) { (void)h; return 0; }
int native_handle_delete(native_handle_t *h) { (void)h; return 0; }
