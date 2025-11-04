#include <iostream>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(){
    //std::cout << "Hello world\n";
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);   

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(8080);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress));

    listen(serverSocket, 5);

    int clientSocket = accept(serverSocket, nullptr, nullptr);

    std::string buffer;
    buffer.resize(1024);
    while (buffer != "EOC"){
        int bytes = recv(clientSocket, buffer.data(), buffer.size(), 0);
        buffer.resize(bytes);
        std::cout << "Message from client: " << buffer << std::endl;
    }

    close(serverSocket);

}
