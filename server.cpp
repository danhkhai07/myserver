#include <cstdio>
#include <netinet/in.h>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <iostream>
#include <queue>
#include <unordered_map>
#include <vector>
#include <algorithm>

#define MAX_CLIENTS 5
#define PORT 65000
#define PACKET_SIZE 16

int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct TCPParser {
private:
    std::queue<int> senderQueue;
    std::queue<int> receiverQueue;
    std::queue<std::vector<char>> messageQueue;

    int msgLen = 0;
    size_t capturedMsgLen = 0;

    int receiverFd = 0;
    size_t capturedReceiverFd = 0;

    std::vector<char> pendingMsg;

    void feed(char* packet, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            char c = packet[i];

            // Parse receiver fd (4 digits)
            if (capturedReceiverFd < 4) {
                if (c < '0' || c > '9') continue;
                receiverFd = c - '0' + receiverFd * 10;
                capturedReceiverFd++;
                continue;
            }

            // Parse message length (4 digits)
            if (capturedMsgLen < 4) {
                if (c < '0' || c > '9') continue;
                msgLen = c - '0' + msgLen * 10;
                capturedMsgLen++;
                continue;
            }

            // Capture message content
            pendingMsg.push_back(c);
            msgLen--;

            // Full message received
            if (msgLen == 0) {
                messageQueue.push(pendingMsg);
                receiverQueue.push(receiverFd);

                // Reset state
                pendingMsg.clear();
                receiverFd = 0;
                capturedReceiverFd = 0;
                capturedMsgLen = 0;
                msgLen = 0;
            }
        }
    }

public:
    struct Message {
        int sender;
        size_t offset = 0;
        std::vector<char> message;
    };

    std::unordered_map<int, std::queue<Message>> openMessageQueue;
    std::unordered_map<int, bool> epollOutFlags;

    void process_msg(int sender, char* buffer, size_t len) {
        feed(buffer, len);
        senderQueue.push(sender);

        while (!senderQueue.empty() && !receiverQueue.empty() && !messageQueue.empty()) {
            Message msg;
            msg.sender = senderQueue.front();
            msg.message = std::move(messageQueue.front());

            int receiver = receiverQueue.front();
            openMessageQueue[receiver].push(std::move(msg));

            senderQueue.pop();
            receiverQueue.pop();
            messageQueue.pop();
        }
    }
};

int main() {
    TCPParser parser;

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(serverFd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0){ 
        perror("bind failed");
        return -1;
    }
    if (listen(serverFd, MAX_CLIENTS) < 0){
        perror("listen failed");
        return -1;
    }
    setNonBlocking(serverFd);

    int epfd = epoll_create1(0);
    struct epoll_event ev{}, events[MAX_CLIENTS];

    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = serverFd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serverFd, &ev);

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_CLIENTS, -1);
        if (nfds < 0) { perror("epoll_wait"); break; }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            // New client
            if (fd == serverFd) {
                int clientFd = accept(serverFd, nullptr, nullptr);
                setNonBlocking(clientFd);
                ev.events = EPOLLIN | EPOLLRDHUP;
                ev.data.fd = clientFd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, clientFd, &ev);
                continue;
            }

            // Client disconnected
            if (events[i].events & EPOLLRDHUP) {
                close(fd);
                parser.openMessageQueue.erase(fd);
                parser.epollOutFlags.erase(fd);
                continue;
            }

            // Readable
            if (events[i].events & EPOLLIN) {
                char buffer[PACKET_SIZE];
                int bytes = read(fd, buffer, sizeof(buffer));
                if (bytes <= 0) continue;
                parser.process_msg(fd, buffer, bytes);
            }

            // Writable
            if (events[i].events & EPOLLOUT) {
                auto& queue = parser.openMessageQueue[fd];
                while (!queue.empty()) {
                    TCPParser::Message& msg = queue.front();
                    int toSend = std::min(PACKET_SIZE, (int)(msg.message.size() - msg.offset));
                    int sent = send(fd, msg.message.data() + msg.offset, toSend, 0);
                    if (sent <= 0) break; // EAGAIN or error

                    msg.offset += sent;
                    if (msg.offset == msg.message.size()) queue.pop();
                }
            }
        }

        // Update EPOLLOUT flags
        for (auto& kv : parser.openMessageQueue) {
            bool wantOut = !kv.second.empty();
            if (wantOut != parser.epollOutFlags[kv.first]) {
                parser.epollOutFlags[kv.first] = wantOut;
                ev.events = EPOLLIN | EPOLLRDHUP;
                if (wantOut) ev.events |= EPOLLOUT;
                ev.data.fd = kv.first;
                epoll_ctl(epfd, EPOLL_CTL_MOD, kv.first, &ev);
            }
        }
    }

    close(serverFd);
    return 0;
}

