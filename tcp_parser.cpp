#include <string>
#include <iostream>

std::string input = "0015kdmcjdhfngl12jx0008judhdhsb0003bbb";

int msgLen = 0;
int capturedMsgLen = 1; // bit dimension
std::string pendingMsg = "";

int splitMessages(std::string packet){
    for (int i = 0; i < packet.size(); i++){
        if (capturedMsgLen <= 8){
            msgLen = packet[i] - 48 + msgLen*10;
            capturedMsgLen = capturedMsgLen << 1;
        } else {
            if (msgLen > 0){
                pendingMsg += packet[i];
                msgLen--;
            } 
            if (msgLen == 0){
                std::cout << pendingMsg << '\n';
                capturedMsgLen = 1;
                pendingMsg = "";
            }
        }
    }
    return 0;
}

int main(){
    int count = 0;
    std::string tmp;
    for (int i = 0; i < input.size(); i++){
        tmp += input[i];
        count++;
        if (count == 3 or i == input.size() - 1){
            splitMessages(tmp);
            count = 0;
            tmp = "";
        } 
    }
    return 0;
}
