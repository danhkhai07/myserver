#include <cstdio>
#include <iostream>

#include <poll.h>
#include <netinet/in.h>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <queue>
#include <utility>

#define MAX_CLIENTS 3
#define PORT 65000

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
        perror("Bind failed");
        return 1;
    }

    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        exit(1);
    }

    if (listen(serverSocket, 5) < 0){
        perror("Listen failed");
        return 1;
    }

    fds[0].fd = serverSocket;
    fds[0].events = POLLIN;

    std::cout << "Server listening on port " << PORT << "...\n";
    
    std::queue<std::pair<int, char*>> messageQ;
    std::queue<std::pair<int, char*>> pending;
    bool reachCap = false;
    while (1){
        int ready = poll(fds, nfds, 0);
        if (ready < 0){
            std::cerr << "Poll error\n";
            break;
        }

        if (fds[0].revents & POLLIN){
            if (nfds < MAX_CLIENTS + 1){
                reachCap = false;
                newSocket = accept(serverSocket, (sockaddr*)&serverAddress, &addrlen); 
                std::cout << "New client connected: " << newSocket << ".\n";
                fds[nfds].fd = newSocket;
                fds[nfds].events = POLLIN;
                nfds++;
            } else {
                if (!reachCap){
                    std::cout << "Reached max capacity.\n";
                    reachCap = true;
                }
            }
        }

        messageQ = pending;
        pending = {};
        for (int i = 1; i < nfds; i++){
            if (fds[i].revents & POLLIN){
                char buffer[1024];
                int bytes = recv(fds[i].fd, buffer, sizeof(buffer), 0);
                buffer[bytes] = '\0';

                if (bytes <= 0){
                    std::cout << "Client " << fds[i].fd << " disconnected.\n";
                    fds[i] = fds[nfds];
                    nfds--;
                    i--;
                    continue;
                }

                pending.push({fds[i].fd, buffer});

            }

            if (messageQ.size() > 0){
                std::queue<std::pair<int, char*>>
                    tmpQ = messageQ;
                while (!tmpQ.empty()){
                    std::pair<int, char*> top = tmpQ.front();
                    std::string msg = "Client " + std::to_string(top.first) + ": " + top.second;
                    send(fds[i].fd, msg.data(), msg.size(), 0);
                    tmpQ.pop();
                }
            }
        }
        messageQ = {};
    }

    close(serverSocket);
    return 0;
}
