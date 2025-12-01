#include <cstddef>
#include <iomanip>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <unordered_set>
#include <cstdint>
#include <ctime>
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

namespace metadata {
    std::unordered_set<int> onlineFds;
    std::unordered_map<int, bool> hasName(false);

    std::unordered_map<std::string, int> nameFdMap;
    std::unordered_map<int, std::string> fdNameMap;

    void removeFdName(int fd){
        
    }

    void addFdName(int fd){

    }
}

namespace miscellaneous {
    const std::string getTime(){
        std::time_t t = std::time(nullptr);
        return std::ctime(&t);
    }
}

namespace container {
    struct Request {
        int sender = -1;
        uint8_t opcode = 0;
        std::string raw;
        std::vector<std::string> tokens;

        Request() = default;
        Request(int sndr, uint8_t op, std::string rw):
             sender(sndr), opcode(op), raw(rw) {}
        virtual ~Request() = default;
    };

    struct Message : public Request {
    public:
        std::unordered_set<int> receiver;
        Message(std::unique_ptr<Request> &req): Request(*req) {};
    };

    enum CommandCode {
        NULL_CMD, 
        P_NAME_REGISTER, 
        P_SEND_MSG, 
        G_CURRENT_ONLINE, 
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

        void reset(){
            *this = Buffer();
        }
    };

    std::queue<std::unique_ptr<container::Request>> completedPackets;
    std::unordered_map<int, std::deque<Buffer>> buffers;

    std::unique_ptr<container::Request> categorize(int sender, uint8_t opcode, std::string raw){
        std::unique_ptr<container::Request> req = std::make_unique<container::Request>(sender, opcode, raw);
        switch (req->opcode){
            case 0: return req;
            case 1: return std::make_unique<container::Message>(req);
            case 2: return std::make_unique<container::Command>(req);
            default:
                std::runtime_error("Unknown opcode\n");
                return nullptr;
        }
    }


public:
    int feed(int sender, std::string_view packet){
        Buffer* buf;
        if (buffers[sender].empty()){
            buffers[sender].push_back(Buffer());
        }
        buf = &buffers[sender].back();

        for (int i = 0; i < packet.size(); ++i){
            char c = packet[i];
            // parse opcode
            if (buf->opParsed < 1){
                buf->opcode = static_cast<uint8_t>(c);
                buf->opParsed++;
                continue;
            }
            // parse length
            if (buf->lenParsed < 2){
                buf->len = (buf->len << 8) | static_cast<uint8_t>(c);
                buf->lenParsed++;
                if (buf->lenParsed >= 2 && buf->len == 0){
                    std::cout << "PacketParser::feed: Warning: Packet with length 0 found.\n";
                    buf->reset();
                }
                continue;
            }

            buf->raw += c;
            buf->len--;
                
            if (buf->len == 0){
                completedPackets.push(categorize(sender, buf->opcode, buf->raw));
                buffers[sender].pop_front();

                if (i + 1 < packet.size()){
                    buffers[sender].push_back(Buffer());
                    buf = &buffers[sender].back();
                }
            }
        }
        
        return 0;
    }

    int getRequest(std::unique_ptr<container::Request> &result){
        if (completedPackets.empty()){
            std::cout << "PacketParser::getRequest: Bad call: Queue is empty.\n";
            return -1;
        }
        result = std::move(completedPackets.front());
        completedPackets.pop();
        return 0;
    }

    bool canRetrieve(){
        return (completedPackets.size() > 0);
    }
};

class Server {
private:
    // TCP Parser var
    PacketParser parser;

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
        clientCount--;
        sendQueue.erase(fd);
        std::string clientName = metadata::fdNameMap[fd];
        metadata::nameFdMap.erase(clientName);
        metadata::fdNameMap.erase(fd);
        metadata::onlineFds.erase(fd);
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
        int nfds = epoll_wait(epfd, events, MAX_CLIENTS + 1, 0);

        while (parser.canRetrieve()){
            std::unique_ptr<container::Request> tmp;
            parser.getRequest(tmp);
            getQueue.push(std::move(tmp));
        }

        for (int i = 0; i < nfds; ++i){ int fd = events[i].data.fd;

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
                clientCount++;
                continue;
            }

            if (events[i].events & EPOLLIN){
                std::string buffer;
                buffer.resize(BUF_SIZE);
                int bytes = read(fd, buffer.data(), buffer.size());
                if (bytes <= 0) {
                    if (bytes == 0) {
                        // connection closed
                        closeClient(fd);
                        std::cout << "Client at fd number " << fd << " disconnected.\n";
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        // real error
                        std::cout << "Read error on fd " << fd << ". Closing client.\n";
                        closeClient(fd);
                    }
                    continue;
                }
                buffer.resize(bytes);

                if (bytes > 0) parser.feed(fd, buffer);
            }

            if (events[i].events & EPOLLOUT){
                if (sendQueue[fd].empty()){
                    // false call
                    epoll_event tmp_ev;
                    tmp_ev.data.fd = fd;
                    tmp_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR; 
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &tmp_ev);
                    continue;
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
    int sendPacket(int fd, std::string msg){
        ToSendMessage sending;
        sending.msg = msg;
        sendQueue[fd].push(sending);
        epoll_event tmp_ev;
        tmp_ev.data.fd = fd;
        tmp_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT;
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
private:
    std::unordered_map<std::string, container::CommandCode> tokenCmdCodeMap = 
        {
            {"/setname",    container::CommandCode::P_NAME_REGISTER},
            {"/msg",        container::CommandCode::P_SEND_MSG},
            {"/onlines",    container::CommandCode::G_CURRENT_ONLINE},
        };

    std::vector<std::string> parseToken(std::string_view raw){
        std::vector<std::string> res;  
        std::string word;
        for (int i = 0; i < raw.size(); i++){
            word += raw[i];
            if (i == raw.size() - 1 || raw[i+1] == ' '){
                res.push_back(word);
                i++;
            }
        }
        return res;
    }

public:
    ///@return 0: no error | -1: raw.empty()
    int handleMessage(Server& server, std::unique_ptr<container::Request>& Req) {
        auto Msg = dynamic_cast<container::Message*>(Req.get());
        std::string msg = Msg->raw;

        //refuse to let unnamed users send messages
        if (!metadata::hasName[Msg->sender]){
            msg = "Cannot send messages while unnamed. To name yourself, use command \"/setname\" as follows:\nUsage: /setname [NAME]";
            server.sendPacket(Msg->sender, msg);
            return 0;
        }

        if (Msg->raw == "") return -1;
        if (Msg->tokens.size() == 0) Msg->tokens = parseToken(Msg->raw);
        

        if (Msg->tokens[0][0] != '/'){
            //global message
            Msg->receiver = metadata::onlineFds;
        } else {
            //personal message
            auto recv = metadata::nameFdMap.find(Msg->tokens[1]);
            if (recv == metadata::nameFdMap.end()){
                msg = "Cannot find user with that name.";
                server.sendPacket(Msg->sender, msg);
                return 0;
            }
            Msg->receiver.insert(recv->second);
            msg = msg.substr(Msg->tokens[0].size(), msg.size() - Msg->tokens[0].size() - 1);
        }
        
        for (int i:Msg->receiver){
            server.sendPacket(i, msg);
        }
        return 0;
    }

    int handleCommand(Server& server, std::unique_ptr<container::Request>& Req) {
        auto Cmd = dynamic_cast<container::Command*>(Req.get());

        if (Cmd->raw == "") return -1;
        if (Cmd->tokens.size() == 0) Cmd->tokens = parseToken(Cmd->raw);

        container::CommandCode cmdCode = container::CommandCode::NULL_CMD;
        {
            auto it = tokenCmdCodeMap.find(Cmd->tokens[0]);
            if (it != tokenCmdCodeMap.end()) cmdCode = it->second;
        }

        std::string msg;
        switch (cmdCode){
            case container::CommandCode::NULL_CMD:
                msg = "Unknown opcode.";
                break;

            case container::CommandCode::P_NAME_REGISTER:
            {
                if (Cmd->tokens.size() < 2 || Cmd->tokens.size() > 2){
                    msg = "Incorrect argument format.\nUsage: /setname [NAME]";
                    break;
                }

                if (metadata::hasName[Cmd->sender]){
                    msg = "Your name has already been set!";
                    break;
                }    

                std::string newUsername = Cmd->tokens[1];
                {
                    auto it = metadata::nameFdMap.find(newUsername);
                    if (it != metadata::nameFdMap.end() ){ 
                        if (it->second != Cmd->sender)
                            msg = "Username is already taken.";
                        else 
                            msg = "Are you trippin'?";
                        break;
                    }
                }

                metadata::addFdName(Cmd->sender);
                msg = "Username successfully set!";
                break;
            }

            case container::CommandCode::P_SEND_MSG:
            {
                if (Cmd->tokens.size() < 3){
                    msg = "Incorrect argument format.\nUsage: /msg [USERNAME] [MESSAGE]";
                    break;
                }
                handleMessage(server, Req);
                break;
            }

            case container::CommandCode::G_CURRENT_ONLINE:
            {    
                if (Cmd->tokens.size() > 1){
                    msg = "Incorrect argument format.\nUsage: /onlines";
                    break;
                }

                msg = "Active list:\n";
                for (int i:metadata::onlineFds){
                    if (metadata::hasName[i]){
                        msg.push_back('\t');
                        msg += metadata::fdNameMap[i];
                        msg.push_back('\n');
                    }
                }
                break;
            }
        }

        server.sendPacket(Cmd->sender, msg);
        return 0;
    }
};

class Dispatcher {
private:
    struct Callback {
        Handler* obj;
        int (Handler::*func)(Server&, std::unique_ptr<container::Request>&);
        int call(Server& server, std::unique_ptr<container::Request>& param){
            return (obj->*func)(server, param);
        }
    };
    std::unordered_map<int, Callback> handlers;
public:
    void registerHandler(uint8_t opcode, Handler* h, int (Handler::*f)(Server&, std::unique_ptr<container::Request>&)){
        Callback cal;
        cal.obj = h;
        cal.func = f;
        handlers[opcode] = cal;
        return;
    }

    int dispatch(Server& server, std::unique_ptr<container::Request> &req){
        auto it = handlers.find(req->opcode);
        if (it == handlers.end()) return -1;
        return it->second.call(server, req);
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
        if (server.canGet()){
            server.getRequest(req);
            if (dispatcher.dispatch(server, req) < 0){;
                server.dropClient(req->sender);
            }
        }
    }
    return 0;
}
