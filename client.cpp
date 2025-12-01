#include <cstdint>
#include <iostream>
#include <cstring>
#include <ostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MSG_SIZE 1024

void receiveMessage(int fd){
    char buffer[MSG_SIZE];
    for (;;){
        int bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes == 0) continue; 
        buffer[bytes] = '\0';
 
        std::cout << "\r" << buffer << "\n";
        std::cout << "> " << std::flush;
    }
}

int main(int argc, char** argv){
    if (argc < 3 || argc > 3){
        std::cout << "Usage: " << argv[0] << " [IPv4] [PORT]\n";
        return -1;
    }
    std::string serverip = argv[1];
    int port = std::stoi(argv[2]);
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);   

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    inet_pton(AF_INET, serverip.c_str(), &serverAddress.sin_addr.s_addr);


    if (connect(clientSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0){
        std::cout << "Connection failed!\n";
        return 1;
    }
    std::cout << "Connection succeeded!\n";

    auto sendMsg = [clientSocket](std::string message){
        if (message.size() > MSG_SIZE - 3 || message.size() < 1){
            std::cout << "Error: Cannot send message of that size.\n";
            return 0;
        }

        uint16_t msgLen = message.size();
        uint8_t opcode;
        if (message[0] != '/') opcode = 1;
        else opcode = 2;
        uint8_t c1 = (msgLen >> 8) & 0xFF;
        uint8_t c2 = msgLen & 0xFF;
        std::string packet;
        packet.push_back(static_cast<uint8_t>(opcode));
        packet.push_back(c1);
        packet.push_back(c2);
        packet += message;
        
        int signal = send(clientSocket, packet.c_str(), packet.size(), 0);
        if (signal <= 0){
            std::cout << "Server went down. Closing program...\n";
            return -1;
        }
        if (message[0] != '/') return 1;
        else return 2;
    };
    
    std::cout << "Note: While prompting, you may lose the current input due to new messages coming in. In that case, keep typing. The message memory isn't lost.\n";
    std::cout << "Type \"/help\" to get started\n";

    std::thread t(receiveMessage, clientSocket);

    std::cout << "> ";
    std::string message;
    while (std::getline(std::cin, message)){
        int signal = sendMsg(message);
        if (signal < 0) return -1;

        if (signal == 1) std::cout << "\33[A\33[K";
        std::cout << "> ";
    }

    close(clientSocket);
    t.join();
}
