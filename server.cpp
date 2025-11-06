#include <iostream>

#include <poll.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 3
#define PORT 1234

int main(){
    int newSocket;
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    pollfd fds[MAX_CLIENTS + 1];
    int nfds = 1;

    sockaddr_in serverAddress;   
    socklen_t addrlen = sizeof(serverAddress);
    serverAddress.sin_family        = AF_INET;
    serverAddress.sin_port          = htons(PORT);
    serverAddress.sin_addr.s_addr   = INADDR_ANY;
        
    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0){
        std::cerr << "Bind failed.";
        return 1;
    }

    if (listen(serverSocket, 5) < 0){
        std::cerr << "Listen failed.";
        return 1;
    }

    fds[0].fd = serverSocket;
    fds[0].events = POLLIN;

    std::cout << "Server listening on port " << PORT << "...\n";
    
    while (1){
        int ready = poll(fds, nfds, 0);
        if (ready < 0){
            std::cerr << "Poll error\n";
            break;
        }

        if (fds[0].revents & POLLIN){
            if (nfds < MAX_CLIENTS + 1){
                newSocket = accept(serverSocket, (sockaddr*)&serverAddress, &addrlen); 
                std::cout << "New client connected: " << newSocket << ".\n";
                fds[nfds].fd = newSocket;
                fds[nfds].events = POLLIN;
                nfds++;
            } else {
                std::cout << "A client tried to connect but got dropped due to maxed out number of clients.\n";
            }
        }

        for (int i = 1; i < nfds; i++){
            if (fds[i].revents & POLLIN){
                char buffer[1024];
                int bytes = recv(fds[i].fd, buffer, sizeof(buffer), 0);

                if (bytes <= 0){
                    std::cout << "Client " << fds[i].fd << " disconnected.\n";
                    fds[i] = fds[nfds];
                    nfds--;
                    i--;
                    continue;
                }

                std::cout << "Client " << fds[i].fd << ": " << buffer << std::endl;
            }
        }
    }

    close(serverSocket);
    return 0;
}
