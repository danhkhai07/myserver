#include <compare>
#include <cstdint>
#include <ctime>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <functional>
#include <cstring>
#include <cstdio>
#include <memory>
#include <iostream>
#include <queue>
#include <deque>
#include <unordered_map>
#include <algorithm>

#define MAX_CLIENTS 5
#define PACKET_SIZE 1024
#define BUF_SIZE 128

namespace container {
    struct Request {
        int sender = -1;
        uint8_t opcode = 0;
        std::string raw;

        Request() = default;
        Request(int sndr, uint8_t op, std::string rw):
             sender(sndr), opcode(op), raw(rw) {}
        virtual ~Request() = default;
    };

    struct Message : public Request {
    private:
        std::string msg;
    public:
        int receiver = -1;

        Message(std::unique_ptr<Request> &req): Request(*req) {};
        std::string getOutput() {
            return msg;
        }
    };

    enum CommandCode {
        NULL_CMD = 0, P_NAME_REGISTER,  
    };

    struct Command : public Request {
    private:
    public:
        CommandCode code = CommandCode::NULL_CMD;

        Command(std::unique_ptr<Request> &req): Request(*req) {};
    };
};

class PacketParser {
private:
    struct Buffer {
        uint8_t lenParsed = 0;
        uint16_t len = 0;

        uint8_t opParsed = 0;
        uint8_t opcode = 0;
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
            // parse opcode
            if (buf->opParsed < 1){
                buf->opcode |= static_cast<uint8_t>(c);
                buf->opParsed++;
                continue;
            }
            // parse length
            if (buf->lenParsed < 2){
                buf->len = (buf->len << 8) | static_cast<uint8_t>(c);
                buf->lenParsed++;
                if (buf->lenParsed >= 2 && buf->len == 0){
                    std::cout << "PacketParser::feed: Warning: Packet with length 0 found.\n";
                    buf->lenParsed = 0;
                    i--;
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
        if (buffers[owner].empty()){
            std::cout << "PacketParser::getRequest: Bad call: Queue is empty.\n";
            return -1;
        }
        Buffer* buf = &buffers[owner].front();
        if (!buf->doneParsed){
            std::cout << "PacketParser::getRequest: Bad call: Incompleted packet.\n";
            return -1;
        }

        result = std::make_unique<container::Request>(owner, buf->opcode, buf->raw);
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
    uint32_t clientCount = 0;
    int serverFd;
    int epfd;
    uint16_t port;
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

    void closeClient(int fd){
        sendQueue.erase(fd);
        close(fd);
    }

public:
    ~Server(){
        if (serverFd >= 0) close(serverFd);
        if (epfd >= 0) close(epfd);
    }

    int initialize(uint16_t p){
        port = p;
        serverFd = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd < 0) return -1;  

        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);
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
                buffer.resize(bytes);

                if (bytes > 0) Parser.feed(fd, buffer);
            }

            if (events[i].events & EPOLLOUT){
                if (sendQueue[fd].empty()){
                    // false call
                    epoll_event tmp_ev;
                    tmp_ev.data.fd = fd;
                    tmp_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR; 
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &tmp_ev);
                }

                ToSendMessage* buffer = &sendQueue[fd].front();
                int messageSize = std::min((int)buffer->msg.size() - buffer->offset, BUF_SIZE);
                int sent = send(fd, buffer->msg.data() + buffer->offset, messageSize, 0);
                if (sent == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    closeClient(fd);
                    std::cout << "Client at fd number " << fd << " disconnected.\n";
                } else {
                    buffer->offset += sent;
                    if (buffer->offset >= buffer->msg.size()) sendQueue[fd].pop();
                }
            }

            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                closeClient(fd);
                std::cout << "Client at fd number " << fd << " disconnected.\n";
            }
        }

        return 0;
    }

    bool canGet(){
        return (!getQueue.empty()); 
    }

    /// The queue stores unique_ptr, so you MUST utilize the result, as queue.front() is popped
    /// immediately after retrieval.
    int getRequest(std::unique_ptr<container::Request>& dest){
        if (!canGet()) return -1;
        dest = std::move(getQueue.front());
        getQueue.pop();
        return 0;
    }

    /// ONLY add the request to queue; the queue is processed in process(). 
    int sendRequest(int fd, std::string msg){
        ToSendMessage sending;
        sending.msg = msg;
        sendQueue[fd].push(sending);
        epoll_event tmp_ev;
        tmp_ev.data.fd = fd;
        tmp_ev.events |= EPOLLOUT;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &tmp_ev);
        return 0;
    }

    int dropClient(int fd){
        std::cout << "Dropping client on fd number " << fd << " due to errors.\n";
        closeClient(fd);
        return 0;
    }
};

class Handler {
public:
    int handleMessage(std::unique_ptr<container::Request>& Msg) {
        std::cout << "handler::handleMessage: Handling \"" << Msg->raw << "\"\n";
        return 0;
    }

    int handleCommand(std::unique_ptr<container::Request>& Cmd) {
        std::cout << "handler::handleCommand: Handling \"" << Cmd->raw << "\"\n";
        return 0;
    }
};

class Dispatcher {
private:
    struct Callback {
        Handler* obj;
        int (Handler::*func)(std::unique_ptr<container::Request>&);
        int call(std::unique_ptr<container::Request>& param){
            return (obj->*func)(param);
        }
    };
    std::unordered_map<int, Callback> handlers;
public:
    void registerHandler(int opcode, Handler* h, int (Handler::*f)(std::unique_ptr<container::Request>&)){
        Callback cal;
        cal.obj = h;
        cal.func = f;
        handlers[opcode] = {h,f};
        return;
    }

    int dispatch(std::unique_ptr<container::Request> &req){
        auto it = handlers.find(req->opcode);
        if (it == handlers.end()) return -1;
        return it->second.call(req);
    }
};

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
    // setting up operational objects
    Server server;
    Dispatcher dispatcher;
    Handler handler;

    dispatcher.registerHandler(1, &handler, &Handler::handleMessage);
    dispatcher.registerHandler(2, &handler, &Handler::handleCommand);

    // logic
    server.initialize(port);
    while(true){
        server.process();
        std::unique_ptr<container::Request> req;
        server.getRequest(req);
        if (dispatcher.dispatch(req) < 0){;
            server.dropClient(req->sender);
        }
    }
    return 0;
}

