#include <iostream>
#include <cstring>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int main(){
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);   

    sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(8080);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
	    perror("Bind failed");
	    return 1;
    }

	if (listen(serverSocket, 5) < 0) {
        perror("Listen failed");
        return 1;
    }

    int clientSocket = accept(serverSocket, nullptr, nullptr);
    std::cout << "Client connected!\n";

    std::string message = "";
    std::string buffer;
    buffer.resize(1024);
    for (;;){
        int bytes = recv(clientSocket, buffer.data(), buffer.size(), 0);
	if (bytes <= 0) break;
	buffer.resize(bytes);
	//std::cout << "Buffer: " << buffer << "._." << std::endl;
	message += buffer;
	//std::cout << "Message: " << message << ' ' << message[message.size()-2] << std::endl;
        if (message.size() >= 1 && message[message.size()-2] == '$') {
            std::cout << "Message from client: " << message << std::endl;
	    message = "";
        }
	buffer.resize(1024);
    }

    close(serverSocket);

}
