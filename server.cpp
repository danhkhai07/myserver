#include <compare>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <cstring>
#include <cstdio>
#include <memory>
#include <iostream>
#include <queue>
#include <deque>
#include <unordered_map>
#include <vector>
#include <algorithm>

#define MAX_CLIENTS 5
#define PACKET_SIZE 1024
#define BUF_SIZE 128
#define MSG_LEN_BYTES 4

namespace container {
    class Request {
    protected:
        int sender;
        std::string raw;
    public:
        Request(int sndr, std::string rw):
             sender(sndr), raw(rw) {}
        virtual ~Request() = default;
        std::string getRaw(){
            return raw;
        }
    };

    class Message : public Request {
    private:
        std::string msg;
    public:
        int receiver;
        std::string getOutput() {
            return msg;
        }
    };

    enum class CommandCode {
        NULL_CMD
    };

    class Command : public Request {
    private:
    public:
        CommandCode code = CommandCode::NULL_CMD;
    };
};

class PacketParser {
private:
    struct Buffer {
        int lenParsed = 0;
        int len = 0;
        std::string raw;
        bool doneParsed = false;

        void reset(){
            *this = Buffer();
        }
    };

    // pair fd & num of completed bufs
    std::unordered_map<int, int> completedNBufs;
    std::unordered_map<int, std::deque<Buffer>> buffers;

public:
    int feed(int owner, std::string_view packet){
        Buffer* buf;
        if (buffers[owner].empty() || buffers[owner].back().doneParsed){
            buffers[owner].push_back(Buffer());
        }
        buf = &buffers[owner].back();

        for (size_t i = 0; i < packet.size(); ++i){
            char c = packet[i];
            if (buf->lenParsed < MSG_LEN_BYTES){
                buf->len = buf->len*10 + c - '0';
                buf->lenParsed++;
                if (buf->lenParsed >= MSG_LEN_BYTES){
                    if (buf->len == 0){
                        std::cout << "PacketParser::feed: Warning: Packet with length 0 found.\n";
                        buf->lenParsed = 0;
                        i--;
                        continue;
                    }
                }
                continue;
            }


            buf->raw += c;
            buf->len--;
               
            if (buf->len == 0){
                buf->doneParsed = true;
                completedNBufs[owner]++;
                if (i + 1 < packet.size()){
                    buffers[owner].push_back(Buffer());
                    buf = &buffers[owner].back();
                }
            }
        }
        
        return 0;
    }

    bool empty(int owner){
        return (completedNBufs[owner] == 0);
    } 

     int getRequest(int owner, std::unique_ptr<container::Request> &result){
        Buffer* buf = &buffers[owner].front();
        if (!buf->doneParsed){
            std::cout << "PacketParser::getRequest: Bad call: Incompleted packet.\n";
            return -1;
        }

        std::unique_ptr<container::Request> tmp = std::make_unique<container::Request>(
            owner,
            buf->raw
        );
        result = std::move(tmp);

        buffers[owner].pop_front();
        completedNBufs[owner]--;
        return 0;
    }
};

class Server {
private:
    // TCP Parser var
    PacketParser Parser;

    // Server shit
    int clientCount = 0;
    int serverFd;
    uint16_t port;
    int epfd;
    sockaddr_in serverAddr;
    epoll_event events[MAX_CLIENTS + 1];

    // TCP queue shit
    struct ToSendMessage {
        int offset = 0;
        std::string msg;
    };
    std::queue<std::unique_ptr<container::Request>> getQueue;
    std::unordered_map<int, std::queue<ToSendMessage>> sendQueue;
    
    int setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) return -1;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void cleanUp(int fd){
        sendQueue.erase(fd);
        close(fd);
    }

public:
    Server(){}
    ~Server(){
        close(serverFd);
    }
    int initialize(int prt){
        serverFd = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd < 0) return -1;  

        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(prt);
        port = prt;
        serverAddr.sin_family = AF_INET;

        if (bind(serverFd, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) return -1;
        if (listen(serverFd, MAX_CLIENTS) < 0) return -1;
        setNonBlocking(serverFd);

        epfd = epoll_create1(0);
        epoll_event tmp_ev;    
        tmp_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
        tmp_ev.data.fd = serverFd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, serverFd, &tmp_ev);

        std::cout << "Server initialized on port: " << port << "\n";
        return 0;
    }

    int process(){
        epoll_event tmp_ev;
        int nfds = epoll_wait(epfd, events, MAX_CLIENTS, 0);

        for (int i = 0; i < nfds; ++i){
            int fd = events[i].data.fd;

            if (fd == serverFd){
                sockaddr_in clientAddr;
                socklen_t addrLen = sizeof(clientAddr);
                int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &addrLen);
                std::string clientIPv4 = inet_ntoa(clientAddr.sin_addr);

                if (clientCount >= MAX_CLIENTS){
                    close(clientFd);
                    std::cout << "IP " << clientIPv4 << " tried to connect but failed due to: (probably maxed capacity)\n";
                    continue;
                }

                tmp_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                tmp_ev.data.fd = clientFd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, clientFd, &tmp_ev);
                std::cout << "IP " << clientIPv4 << " connected through fd number " << clientFd << ".\n";
            }

            if (events[i].events & EPOLLIN){
                std::string buffer;
                buffer.resize(BUF_SIZE);
                size_t bytes = read(fd, buffer.data(), buffer.size());
                buffer.resize(bytes + 1);

                std::cout << buffer << '\n';
                if (bytes > 0) Parser.feed(fd, buffer);
            }

            if (events[i].events & EPOLLOUT){
                ToSendMessage* buffer = &sendQueue[fd].front();
                int messageSize = std::min((int)buffer->msg.size() - buffer->offset, BUF_SIZE);
                int sent = send(fd, buffer->msg.data() + buffer->offset, BUF_SIZE, 0);
                buffer->offset += sent;
                if (buffer->offset >= buffer->msg.size()) sendQueue[fd].pop();
            }

            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                cleanUp(fd);
                std::cout << "Client at fd number " << fd << " disconnected.\n";
            }
        }

        return 0;
    }

    /// The queue stores unique_ptr, so you MUST utilize the result, as queue.front() is popped
    /// immediately after retrieval.
    int getRequest(std::unique_ptr<container::Request>& result){
        result = std::move(getQueue.front());
        getQueue.pop();
        return 0;
    }

    /// ONLY add the request to queue; the queue is processed in process(). 
    int sendRequest(int fd, std::string msg){
        ToSendMessage sending;
        sending.msg = msg;
        sendQueue[fd].push(sending);
        return 0;
    }
};

namespace dispatcher {
    
}

namespace handler {

}

int main(int argc, char** argv){
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " [PORT]\n";
        return -1;
    }

    if (argc > 2) {
        std::cout << "Error: Too many arguments.\n";
        std::cout << "Usage: " << argv[0] << " [PORT]\n";
        return -1;
    }
    int port = std::stoi(argv[1]);

    // manager's work
    Server server;
    server.initialize(port);
    while(true){
        server.process();
    }
    return 0;
}

