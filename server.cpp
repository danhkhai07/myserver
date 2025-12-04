#include <cerrno>
#include <cstddef>
#include <iterator>
#include <netinet/in.h>
#include <queue>
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
#include <vector>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <algorithm>

#define MAX_CLIENTS 5
#define PACKET_SIZE 1024
#define BUF_SIZE 128

namespace metadata {
    std::unordered_set<int> onlineFds;

    std::unordered_map<int, bool> hasName;
    std::unordered_map<std::string, int> nameFdMap;
    std::unordered_map<int, std::string> fdNameMap;

    int removeFdName(int fd){
        if (!hasName[fd]) return 0;
        auto it = fdNameMap.find(fd);      
        if (it != fdNameMap.end()){
            nameFdMap.erase(it->second);
            fdNameMap.erase(fd);
            hasName.erase(fd);
        } else {
            std::cout << "metadata::removeFdName: Cannot find fd " << fd << ".\n";
            return -1;
        }
        return 0;
    }

    int addFdName(int fd, const char* name, uint16_t size){
        if (hasName[fd]){ 
            std::cout << "metadata::addFdName: Client " << fd << " already has a name by " << fdNameMap[fd] <<".\n";
            return -1;
        }

        if (size < 4 || size > 15){
            std::cout << "metadata::addFdName: Client " << fd << "'s choice of name is out of \"character\".\n";
            return -1;
        }

        auto it = nameFdMap.find(name);
        if (it != nameFdMap.end()){
            std::cout << "metadata::addFdName: " << name << " already exists.\n";
            return -1;
        }

        fdNameMap[fd] = name;
        nameFdMap[name] = fd;
        hasName[fd] = true;
        return 0;
    }
}

namespace miscellaneous {
    const std::string getTime(){
        std::time_t t = std::time(nullptr);
        return std::ctime(&t);
    }
}

namespace container {
    enum CommandCode {
        NULL_CMD, 
        P_NAME_REGISTER, 
        P_SEND_MSG, 
        G_CURRENT_ONLINE, 
        G_HELP
    };

    struct Request {
        int sender = -1;
        uint8_t opcode = 0;
        char raw[PACKET_SIZE];
        uint16_t len = 0;
        std::vector<std::string> tokens;
        std::unordered_set<int> receiver;
        CommandCode code = CommandCode::NULL_CMD;

        Request() = default;
        Request(int sndr, uint8_t op, char* rw, uint16_t ln):
            sender(sndr), opcode(op), len(ln)
        {
            std::memcpy(raw, rw, ln);
        }
        Request(int sndr, char* rw, uint16_t ln, std::vector<std::string> tkns):
            sender(sndr), len(ln), tokens(tkns)
        {
            std::memcpy(raw, rw, ln);
        }

        ~Request() = default;
    };

    template<typename T, uint16_t K>
    struct RingQueue {
    private:
        static const uint16_t size = K;
        T queue[size + 1];
        int head = 0, tail = 0;
    public:
        bool empty(){
            return (head == tail);
        }

        bool full(){
            return ((tail + 1) % (size + 1) == head);
        }

        uint16_t getSize(){
            return (tail - head + size + 1) % (size + 1);
        }

        T& front(){
            return queue[head];
        }

        T& back(){
            return queue[(tail + size) % (size + 1)];
        }

        bool push_front(const T &in){
            int nextHead = (head + size) % (size + 1);
            if (this->full()) return false;
            queue[nextHead] = in;
            head = nextHead;
            return true;
        }

        bool push_back(const T &in){
            int nextTail = (tail + 1) % (size + 1);
            if (this->full()) return false;
            queue[tail] = in;
            tail = nextTail;
            return true;
        }

        bool pop_front(){
            if (this->empty()) return false;
            head = (head + 1) % (size + 1);
            return true;
        }

        bool pop_back(){
            if (this->empty()) return false;
            tail = (tail + size) % (size + 1);
            return true;
        }
    };
};

class PacketParser {
private:
    struct Buffer {
        uint8_t lenParsed = 0;
        uint16_t len = 0;

        uint8_t opParsed = 0;
        uint8_t opcode = 0;
        char raw[PACKET_SIZE];

        void reset(){
            *this = Buffer();
        }
    };

    container::RingQueue<container::Request, 8> completedPackets;
    std::unordered_map<int, container::RingQueue<Buffer, 4>> buffers;

public:
    int feed(int sender, char* packet, uint16_t len){
        Buffer* buf;
        if (buffers[sender].empty()){
            buffers[sender].push_back(Buffer());
        }
        buf = &buffers[sender].back();
            
        char* it = packet;   
        uint16_t tmp_len = 0;
        for (int i = 0; i < len; ++i, ++it){
            char c = *it;
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

                if (buf->lenParsed >= 2){
                    if (buf->len == 0){
                        std::cout << "PacketParser::feed: Warning: Packet with length 0 found.\n";
                        buf->reset();
                    }
                    if (buf->len > PACKET_SIZE){
                        std::cout << "PacketParser::feed: Client " << sender << " sent an oversized packet (>1024 bytes). Returning an error.\n";
                        return -1;
                    }
                }
                continue;
            }

            buf->raw[tmp_len] = c;
            tmp_len++;
                
            if (tmp_len >= buf->len){
                tmp_len = 0;
                container::Request req = container::Request(sender, buf->opcode, buf->raw, buf->len);
                completedPackets.push_back(req);
                buffers[sender].pop_front();

                if (i + 1 < len){
                    buffers[sender].push_back(Buffer());
                    buf = &buffers[sender].back();
                }
            }
        }
        
        return 0;
    }

    int getRequest(container::Request &result){
        if (completedPackets.empty()){
            std::cout << "PacketParser::getRequest: Bad call: Queue is empty.\n";
            return -1;
        }
        result = completedPackets.front();
        completedPackets.pop_front();
        return 0;
    }

    bool canRetrieve(){
        return (completedPackets.getSize() > 0);
    }
};

class Server {
private:
    const uint16_t MAX_READ_VOLUME = 4096;

    // TCP Parser var
    PacketParser parser;

    // Server shit
    uint32_t clientCount = 0;
    int serverFd = -1;
    int epfd = -1;
    uint16_t port = 0;
    sockaddr_in serverAddr;
    epoll_event events[MAX_CLIENTS + 1];

    // TCP queue shit
    struct ToSendMessage {
        int offset = 0;
        uint16_t len = 0;
        char msg[PACKET_SIZE];
    };
    container::RingQueue<container::Request, 8> getQueue;
    std::unordered_map<int, container::RingQueue<ToSendMessage, 8>> sendQueue;
    container::RingQueue<int, MAX_CLIENTS> undrainedFds;
    
    int setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) return -1;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void addClient(int fd){
        clientCount++;
        metadata::onlineFds.insert(fd);
    }

    void closeClient(int fd){
        clientCount--;
        sendQueue.erase(fd);
        metadata::removeFdName(fd);
        metadata::onlineFds.erase(fd);
        close(fd);
    }

    bool handleEPOLLIN(int fd){
        bool continueFlag = false;
        bool drained = false;

        char buffer[BUF_SIZE];
        uint16_t totalRead = 0;

        while (totalRead <= MAX_READ_VOLUME){
            int bytes = read(fd, buffer, sizeof(buffer));
            if (bytes > 0){
                if (parser.feed(fd, buffer, bytes) < 0){
                    std::cout << "Server::process: An error occur when feeding packets of client " << fd << ".\n";
                    std::cout << "Server::process: Dropping client " << fd << ".\n";
                    return true;
                }
                totalRead += bytes;
                continue;
            }

            if (bytes == 0) {
                closeClient(fd);
                std::cout << "Server::process: Client at fd number " << fd << " disconnected.\n";
                return true;
            }

            if (errno == EAGAIN){
                drained = true;
                break;
            }

            std::cout << "Server::process: Read error on fd " << fd << ". Closing client.\n";
            closeClient(fd);
            return true;
        }
        
        if (!continueFlag && !drained){
            undrainedFds.push_back(fd);
        }
        return continueFlag;
    }
public:
    ~Server(){
        if (serverFd >= 0) close(serverFd);
        if (epfd >= 0) close(epfd);
    }

    int initialize(uint16_t p){
        port = p;
        serverFd = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd < 0){
            perror("serverFd");
            return -1;
        }
        if (serverFd < 0) return -1;  

        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);
        serverAddr.sin_family = AF_INET;

        if (bind(serverFd, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) return -1;
        if (listen(serverFd, MAX_CLIENTS) < 0) return -1;
        setNonBlocking(serverFd);

        epfd = epoll_create1(0);
        if (epfd < 0){
            perror("epoll_create1");
            return -1;
        }
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
            container::Request tmp;
            parser.getRequest(tmp);
            getQueue.push_back(tmp);
        }

        for (int i = 0; i < nfds; ++i){ 
            int fd = events[i].data.fd;

            if (fd == serverFd){
                sockaddr_in clientAddr;
                socklen_t addrLen = sizeof(clientAddr);
                int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &addrLen);
                std::string clientIPv4 = inet_ntoa(clientAddr.sin_addr);

                if (clientCount >= MAX_CLIENTS){
                    close(clientFd);
                    std::cout << "Server::process: IP " << clientIPv4 << " tried to connect but failed due to maxed capacity.\n";
                    continue;
                }
                
                setNonBlocking(clientFd);
                tmp_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
                tmp_ev.data.fd = clientFd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, clientFd, &tmp_ev);
                std::cout << "Server::process: IP " << clientIPv4 << " connected through fd number " << clientFd << ".\n";
                addClient(clientFd);
                continue;
            }

            if (events[i].events & EPOLLIN){
                if (handleEPOLLIN(fd)) continue;
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
                int messageSize = std::min((int) buffer->len - buffer->offset, BUF_SIZE);
                int sent = send(fd, buffer->msg + buffer->offset, messageSize, 0);
                if (sent == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    closeClient(fd);
                    std::cout << "Server::process: Client at fd number " << fd << " disconnected.\n";
                } else {
                    buffer->offset += sent;
                    if (buffer->offset >= buffer->len) sendQueue[fd].pop_front();
                }
            }

            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                closeClient(fd);
                std::cout << "Server::process: Client at fd number " << fd << " disconnected.\n";
            }
        }

        for (int i = 0; i < undrainedFds.getSize(); i++){
            int fd = undrainedFds.front();
            undrainedFds.pop_front();
            if (handleEPOLLIN(fd)) continue;
        }

        return 0;
    }

    bool canGet(){
        return (!getQueue.empty()); 
    }

    /// The queue stores unique_ptr, so you MUST utilize the result, as queue.front is popped
    /// immediately after retrieval. int getRequest(std::unique_ptr<container::Request>& dest){
    int getRequest(container::Request& dest){
        if (!canGet()) return -1;
        dest = getQueue.front();
        getQueue.pop_front();
        return 0;
    }

    /// ONLY add the request to queue; the queue is processed in process(). 
    int sendPacket(int fd, char* msg, uint16_t len){
        ToSendMessage sending;
        std::memcpy(sending.msg, msg, len);
        sending.len = len;
        //std::cout << "Sending " << sending.msg << "...\n";
        sendQueue[fd].push_back(sending);
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
    const std::unordered_map<std::string, container::CommandCode> tokenCmdCodeMap = 
        {
            {"/setname",    container::CommandCode::P_NAME_REGISTER},
            {"/msg",        container::CommandCode::P_SEND_MSG},
            {"/onlines",    container::CommandCode::G_CURRENT_ONLINE},
            {"/help",       container::CommandCode::G_HELP},
        };

    void appendChar(const char* s, char* out, uint16_t& len){
        size_t n = strlen(s);
        std::memcpy(out + len, s, n);
        len += n;
    }

    std::vector<std::string> parseToken(std::string_view raw){
        std::vector<std::string> res;
        char w[PACKET_SIZE];
        uint16_t w_len = 0;
        for (char c : raw){
            if (c == ' '){
                if (w_len > 0){
                    res.push_back(w);
                    w[0] = '\0';
                    w_len = 0;
                }
            } else {
                w[w_len] = c;
                w_len++;
            }
        }
        if (w_len > 0){
            res.push_back(w);
        }
        return res;
    }

public:
    ///@return 0: no error | -1: raw.empty()
    int handleMessage(Server& server, container::Request& Req) {
        char msg[PACKET_SIZE + 64]; //padding for extra characters 
        uint16_t len = 0;

        //refuse to let unnamed users send messages
        if (!metadata::hasName[Req.sender]){
            appendChar(
                "[SERVER] Cannot send messages while unnamed. To name yourself, use command \"/setname\" as follows:\nUsage: /setname [NAME]",
                msg, len);
                
            server.sendPacket(Req.sender, msg, len);
            return 0;
        }

        if (Req.len == 0) return -1;
        if (Req.tokens.size() == 0) Req.tokens = parseToken(Req.raw);
        

        if (Req.tokens[0][0] != '/'){
            //global message
            Req.receiver = metadata::onlineFds;
            appendChar("[GLOBAL] ", msg, len);
            appendChar(metadata::fdNameMap[Req.sender].c_str(), msg, len);
            appendChar(": ", msg, len);
            appendChar(Req.raw, msg, len);
        } else {
            //personal message
            Req.receiver.insert(Req.sender);
            auto recv = metadata::nameFdMap.find(Req.tokens[1]);
            if (recv == metadata::nameFdMap.end()){
                appendChar("[SERVER] Cannot find user with that name.", msg, len);
                server.sendPacket(Req.sender, msg, len);
                return 0;
            }

            if (recv->second == Req.sender){
                appendChar( "[SERVER] Cannot send message to yourself.", msg, len);
                server.sendPacket(Req.sender, msg, len);
                return 0;
            }
            Req.receiver.insert(recv->second);
            int redundant = Req.tokens[0].size() + Req.tokens[1].size() + 2;
            char tmp_msg[1024];
            std::memcpy(tmp_msg, msg + redundant, len);

            char custom[PACKET_SIZE];
            uint16_t custom_len = 0;
            appendChar("[PERSONAL] ", custom, custom_len);
            appendChar(metadata::fdNameMap[Req.sender].c_str(), custom, custom_len);
            appendChar(": ", custom, custom_len);
            appendChar(tmp_msg, custom, custom_len);

            std::memcpy(msg, custom, custom_len);
            len = strlen(msg);
        }
        
        for (int i:Req.receiver){
            server.sendPacket(i, msg, len);
        }
        return 0;
    }

    int handleCommand(Server& server, container::Request& Req) {
        if (Req.len == 0) return -1;
        if (Req.tokens.size() == 0) Req.tokens = parseToken(Req.raw);

        char msg[PACKET_SIZE + 64]; //padding for extra characters 
        uint16_t len = 0;

        container::CommandCode cmdCode = container::CommandCode::NULL_CMD;
        {
            auto it = tokenCmdCodeMap.find(Req.tokens[0]);
            if (it != tokenCmdCodeMap.end()) cmdCode = it->second;
        }

        switch (cmdCode){
            case container::CommandCode::P_NAME_REGISTER:
            {
                if (Req.tokens.size() < 2 || Req.tokens.size() > 2){
                    appendChar("[SERVER] Incorrect argument format.\nUsage: /setname [NAME]", msg, len);
                    break;
                }

                if (metadata::hasName[Req.sender]){
                    appendChar("[SERVER] Your name has already been set!", msg, len);
                    break;
                }    

                std::string newUsername = Req.tokens[1];
                {
                    auto it = metadata::nameFdMap.find(newUsername);
                    if (it != metadata::nameFdMap.end() ){ 
                        if (it->second != Req.sender)
                            appendChar("[SERVER] Username is already taken.", msg, len);
                        else 
                            appendChar("[SERVER] Are you trippin'?", msg, len);
                        break;
                    }
                }

                metadata::addFdName(Req.sender, newUsername.c_str(), newUsername.size());
                appendChar("[SERVER] Username successfully set!", msg, len);
                break;
            }

            case container::CommandCode::P_SEND_MSG:
            {
                if (Req.tokens.size() < 3){
                    appendChar("[SERVER] Incorrect argument format.\nUsage: /msg [USERNAME] [MESSAGE]", msg, len);
                    break;
                }
                return handleMessage(server, Req);
            }

            case container::CommandCode::G_CURRENT_ONLINE:
            {    
                if (Req.tokens.size() > 1){
                    appendChar("[SERVER] Incorrect argument format.\nUsage: /onlines", msg, len);
                    break;
                }

                appendChar("[SERVER] Active list:\n", msg, len);
                for (int i:metadata::onlineFds){
                    if (metadata::hasName[i]){
                        appendChar("\t", msg, len);
                        appendChar(metadata::fdNameMap[i].c_str(), msg, len);
                        appendChar("\n", msg, len);
                    }
                }
                msg[len - 1] = '\0'; // pop the last \n char
                len--;
                break;
            }
            
            case container::CommandCode::G_HELP:
            {
                appendChar("To chat globally, simply type directly after '>' symbol.\nAvailable commands include:\n    /help\t\t\t: Display this text.\n    /setname [NAME]\t\t: Set your own name before texting.\n    /onlines\t\t\t: Display currently online users.\n    /msg [NAME] [MESSAGE]\t: Message privately with an active user.", msg, len);
                // To chat globally, simply type directly after '>' symbol.\n
                // Available commands include:\n
                //     /help\t\t\t: Display this text.\n
                //     /setname [NAME]\t\t: Set your own name before texting.\n
                //     /onlines\t\t\t: Display currently online users.\n
                //     /msg [NAME] [MESSAGE]\t: Message privately with an active user.\n
                break;
            }

            case container::CommandCode::NULL_CMD:
                appendChar("[SERVER] Unknown command.", msg, len);
                break;
        }

        server.sendPacket(Req.sender, msg, len);
        return 0;
    }
};

class Dispatcher {
private:
    struct Callback {
        Handler* obj;
        int (Handler::*func)(Server&, container::Request&);
        int call(Server& server, container::Request& param){
            return (obj->*func)(server, param);
        }
    };
    std::unordered_map<int, Callback> handlers;
public:
    void registerHandler(uint8_t opcode, Handler* h, int (Handler::*f)(Server&, container::Request&)){
        Callback cal;
        cal.obj = h;
        cal.func = f;
        handlers[opcode] = cal;
        return;
    }

    int dispatch(Server& server, container::Request& req){
        auto it = handlers.find(req.opcode);
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
        container::Request req;
        if (server.canGet()){
            server.getRequest(req);
            if (dispatcher.dispatch(server, req) < 0){;
                server.dropClient(req.sender);
            }
        }
    }
    return 0;
}
