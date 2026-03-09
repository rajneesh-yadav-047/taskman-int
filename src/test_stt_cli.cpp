#include "stt_ws_client.h"
#include <iostream>
int main(){
    bool ok = stream_file_to_stt_ws(std::wstring(L"127.0.0.1"), 8001, std::wstring(L"/ws/stt"), std::string("capture.wav"), NULL);
    std::cout << "stream returned: " << ok << "\n";
    return 0;
}
