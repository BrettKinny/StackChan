// AI-authored by Claude Opus 5.5 (Anthropic) for #200. Runs the real esp-ml307
// WebSocket + EspTcp sources on host threads and replays the wake-from-sleep
// race: the server's close frame is being handled on the TCP receive task when
// another task replaces the WebSocket (WebsocketProtocol::OpenAudioChannel's
// `websocket_ = CreateWebSocket()`). Built with AddressSanitizer: without the
// teardown fix the receive task writes into the destroyed receive buffer.
#include "esp_tcp.h"
#include "network_interface.h"
#include "web_socket.h"

#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <sys/socket.h>
#include <thread>

namespace {
std::mutex m;
std::condition_variable cv;
bool handshake_sent = false, send_close = false, fd_closed = false, disconnected = false;
int recv_phase = 0;
constexpr int kFd = 42;

void notify(bool& flag) {
    {
        std::lock_guard<std::mutex> lock(m);
        flag = true;
    }
    cv.notify_all();
}

void wait_for(const bool& flag) {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return flag; });
}

class HostNetwork : public NetworkInterface {
public:
    std::unique_ptr<Http> CreateHttp(int) override { return nullptr; }
    std::unique_ptr<Tcp> CreateTcp(int) override { return std::make_unique<EspTcp>(); }
    std::unique_ptr<Tcp> CreateSsl(int) override { return nullptr; }
    std::unique_ptr<Udp> CreateUdp(int) override { return nullptr; }
    std::unique_ptr<Mqtt> CreateMqtt(int) override { return nullptr; }
    std::unique_ptr<WebSocket> CreateWebSocket(int id) override { return std::make_unique<WebSocket>(this, id); }
};
}  // namespace

extern "C" hostent* __wrap_gethostbyname(const char*) {
    static char address[4] = {127, 0, 0, 1};
    static char* addresses[] = {address, nullptr};
    static hostent entry = {nullptr, nullptr, AF_INET, 4, addresses};
    return &entry;
}
extern "C" int __wrap_socket(int, int, int) { return kFd; }
extern "C" int __wrap_connect(int, const sockaddr*, socklen_t) { return 0; }
extern "C" int __wrap_close(int fd) {
    assert(fd == kFd);
    notify(fd_closed);
    return 0;
}
extern "C" ssize_t __wrap_send(int, const void* data, size_t size, int) {
    if (size >= 4 && std::memcmp(data, "GET ", 4) == 0) notify(handshake_sent);
    return size;
}
extern "C" ssize_t __wrap_recv(int, void* buffer, size_t size, int) {
    // A handshake response longer than the SSO buffer, so receive_buffer_
    // owns heap storage, as it does on the robot after the server hello.
    static const char kHandshake[] =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
    static const char kCloseFrame[] = {'\x88', '\x00'};
    if (recv_phase == 0) {
        wait_for(handshake_sent);
        recv_phase = 1;
        std::memcpy(buffer, kHandshake, sizeof(kHandshake) - 1);
        return sizeof(kHandshake) - 1;
    }
    if (recv_phase == 1) {
        wait_for(send_close);
        recv_phase = 2;
        assert(size >= sizeof(kCloseFrame));
        std::memcpy(buffer, kCloseFrame, sizeof(kCloseFrame));
        return sizeof(kCloseFrame);
    }
    wait_for(fd_closed);  // a blocked recv() returns once the socket is closed
    errno = EBADF;
    return -1;
}

int main() {
    HostNetwork network;
    auto* ws = new WebSocket(&network, 1);
    ws->OnDisconnected([] {
        // xiaozhi's handler: log, SetPowerSaveLevel, Schedule(...). The
        // Schedule wakes the main task, which preempts this priority-1 task.
        notify(disconnected);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });
    assert(ws->Connect("ws://127.0.0.1:8000/xiaozhi/v1/"));
    assert(ws->IsConnected());

    notify(send_close);       // server end_session close frame arrives
    wait_for(disconnected);   // ...and the receive task is inside on_disconnected_
    assert(!ws->IsConnected());
    delete ws;                // wake event reopens the channel, replacing the socket
    assert(fd_closed);
    std::puts("websocket teardown: ok");
    return 0;
}
