#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(){
    std::cout << "Note: Send 'EOC' to end conversation.\n";
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);   

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(1234);
    serverAddress.sin_addr.s_addr = inet_addr("36.50.55.225");

    if (connect(clientSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress))){
        std::cout << "Connection failed!\n";
        return 1;
    }

    std::string message;
    for (;;){
        std::cout << "Message to server: ";
        std::getline(std::cin, message);
        send(clientSocket, message.c_str(), message.size() + 1, 0);
    }

    close(clientSocket);
}
