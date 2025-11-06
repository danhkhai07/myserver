#include <iostream>
#include <cstring>
#include <ostream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 14007

void receiveMessage(int fd){
    char buffer[1024];
    for (;;){
        int bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) return;
        buffer[bytes] = '\0';
        std::cout << "\r" << buffer << "\n> " << std::flush;
    }
}

int main(){
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);   

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_addr.s_addr = inet_addr("36.50.55.225");

    if (connect(clientSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0){
        std::cout << "Connection failed!\n";
        return 1;
    }
    std::cout << "Connection succeeded!\n";

    std::cout << "> ";
    std::thread t(receiveMessage, clientSocket);

    std::string message;
    while (std::getline(std::cin, message)){
        std::cout << "\033[A\033[K";
        send(clientSocket, message.c_str(), message.size() + 1, 0);
    }

    close(clientSocket);
    t.join();
}
