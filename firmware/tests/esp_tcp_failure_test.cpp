// AI-assisted by GPT-6 (OpenAI). Executes EspTcp's actual source with host fakes.
#include "esp_tcp.h"
#include <cassert>
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

bool task_creation_succeeds = false;
int wait_count = 0, task_create_count = 0;
int close_count = 0, send_count = 0;
static int open_fd = -1;

extern "C" hostent* __wrap_gethostbyname(const char*) {
    static char address[4] = {127, 0, 0, 1};
    static char* addresses[] = {address, nullptr};
    static hostent entry = {nullptr, nullptr, AF_INET, 4, addresses};
    return &entry;
}
extern "C" int __wrap_socket(int, int, int) { open_fd = 42; return open_fd; }
extern "C" int __wrap_connect(int fd, const sockaddr*, socklen_t) { assert(fd == open_fd); return 0; }
extern "C" int __wrap_close(int fd) { assert(fd == open_fd); ++close_count; open_fd = -1; return 0; }
extern "C" ssize_t __wrap_send(int fd, const void*, size_t size, int) {
    assert(fd == open_fd); ++send_count; return size;
}
extern "C" ssize_t __wrap_recv(int, void*, size_t, int) { return 0; }

int main() {
    {
        EspTcp tcp;
        assert(!tcp.Connect("localhost", 80));
        assert(tcp.GetLastError() == ENOMEM);
        assert(!tcp.connected());
        assert(open_fd == -1 && close_count == 1 && task_create_count == 1);
        assert(tcp.Send("must not send") == -1 && send_count == 0);
        tcp.Disconnect();
        assert(wait_count == 0 && close_count == 1);

        // The same instance must recover once task allocation succeeds.
        task_creation_succeeds = true;
        assert(tcp.Connect("localhost", 80));
        assert(tcp.connected() && tcp.GetLastError() == 0);
        assert(tcp.Send("ok") == 2 && send_count == 1);
        tcp.Disconnect();
        assert(!tcp.connected() && open_fd == -1);
        assert(close_count == 2 && wait_count == 1 && task_create_count == 2);
    }
    assert(close_count == 2 && wait_count == 1);  // Destructor is idempotent.
    task_creation_succeeds = false;
    {
        EspTcp tcp;
        assert(!tcp.Connect("localhost", 80));
    }
    assert(close_count == 3 && wait_count == 1);  // Failed-instance destruction never waits.
}
