#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Conn {
    int fd{-1};
    bool connected{false};
};

bool set_non_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    return true;
}

struct Options {
    std::string host{"127.0.0.1"};
    int port{9000};
    int connections{100000};
    int duration_sec{30};
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog
              << " [--host 127.0.0.1] [--port 9000] [--connections 100000] [--duration 30]\n";
}

Options parse_args(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            opt.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            opt.port = std::atoi(argv[++i]);
        } else if (arg == "--connections" && i + 1 < argc) {
            opt.connections = std::atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            opt.duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    if (opt.port <= 0 || opt.port > 65535) {
        throw std::runtime_error("invalid port");
    }
    if (opt.connections <= 0) {
        throw std::runtime_error("connections must be > 0");
    }
    if (opt.duration_sec <= 0) {
        throw std::runtime_error("duration must be > 0");
    }
    return opt;
}

int create_and_connect(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (!set_non_blocking(fd)) {
        ::close(fd);
        return -1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return -1;
    }

    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
        return fd;
    }
    if (r < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    try {
        Options opt = parse_args(argc, argv);

        std::cout << "Echo QPS benchmark\n";
        std::cout << "  target: " << opt.host << ":" << opt.port << "\n";
        std::cout << "  connections: " << opt.connections << "\n";
        std::cout << "  duration: " << opt.duration_sec << " seconds\n";
        std::cout << "Note: ensure echo server is running and system ulimit/net settings allow enough FDs.\n";

        int ep = ::epoll_create1(0);
        if (ep < 0) {
            throw std::runtime_error("epoll_create1 failed");
        }

        std::vector<Conn> conns;
        conns.reserve(static_cast<std::size_t>(opt.connections));

        const std::string payload = "ping\n";
        std::uint64_t total_responses = 0;

        // 建立所有连接
        for (int i = 0; i < opt.connections; ++i) {
            int fd = create_and_connect(opt.host, opt.port);
            if (fd < 0) {
                std::cerr << "Failed to create/connect socket at index " << i
                          << ": " << std::strerror(errno) << "\n";
                break;
            }
            Conn c;
            c.fd = fd;
            c.connected = false;

            epoll_event ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.data.u32 = static_cast<uint32_t>(i);
            ev.events = EPOLLIN | EPOLLOUT;
            if (::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
                std::cerr << "epoll_ctl ADD failed: " << std::strerror(errno) << "\n";
                ::close(fd);
                break;
            }

            conns.push_back(c);
        }

        int active = static_cast<int>(conns.size());
        if (active == 0) {
            std::cerr << "No active connections created, exit.\n";
            ::close(ep);
            return 1;
        }

        const int max_events = 1024;
        std::vector<epoll_event> events(static_cast<std::size_t>(max_events));

        auto start = std::chrono::steady_clock::now();
        auto end_time = start + std::chrono::seconds(opt.duration_sec);

        std::vector<char> buf(4096);

        while (active > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time) {
                break;
            }
            int timeout_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now).count());
            if (timeout_ms < 0) timeout_ms = 0;

            int n = ::epoll_wait(ep, events.data(), max_events, timeout_ms);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::cerr << "epoll_wait error: " << std::strerror(errno) << "\n";
                break;
            }
            if (n == 0) {
                continue;
            }

            for (int i = 0; i < n; ++i) {
                uint32_t idx = events[static_cast<std::size_t>(i)].data.u32;
                if (idx >= conns.size()) continue;
                Conn& c = conns[idx];
                if (c.fd < 0) continue;

                uint32_t ev = events[static_cast<std::size_t>(i)].events;

                if (ev & (EPOLLHUP | EPOLLERR)) {
                    ::close(c.fd);
                    c.fd = -1;
                    --active;
                    continue;
                }

                if (!c.connected && (ev & EPOLLOUT)) {
                    // 完成 connect，并立即发送第一条 ping
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                        ::close(c.fd);
                        c.fd = -1;
                        --active;
                        continue;
                    }
                    c.connected = true;
                    ssize_t sent = ::send(c.fd, payload.data(), payload.size(), 0);
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        ::close(c.fd);
                        c.fd = -1;
                        --active;
                        continue;
                    }
                }

                if (ev & EPOLLIN) {
                    for (;;) {
                        ssize_t r = ::recv(c.fd, buf.data(), buf.size(), 0);
                        if (r > 0) {
                            // 假设服务器严格 echo。
                            total_responses++;
                            // 立即再发一条。
                            ssize_t sent = ::send(c.fd, payload.data(), payload.size(), 0);
                            (void)sent;
                            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                                ::close(c.fd);
                                c.fd = -1;
                                --active;
                                break;
                            }
                            // 继续读，直到 EAGAIN。
                            continue;
                        } else if (r == 0) {
                            ::close(c.fd);
                            c.fd = -1;
                            --active;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            ::close(c.fd);
                            c.fd = -1;
                            --active;
                            break;
                        }
                    }
                }
            }
        }

        auto finish = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(finish - start).count();

        for (std::size_t i = 0; i < conns.size(); ++i) {
            if (conns[i].fd >= 0) {
                ::close(conns[i].fd);
            }
        }
        ::close(ep);

        double qps = (seconds > 0.0) ? static_cast<double>(total_responses) / seconds : 0.0;

        std::cout << "Total responses: " << total_responses << "\n";
        std::cout << "Total time: " << seconds << " s\n";
        std::cout << "QPS: " << qps << "\n";

        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}

