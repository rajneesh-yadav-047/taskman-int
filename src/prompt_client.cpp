#include "prompt_client.h"
#include "http_client.h"
#include <iostream>

std::string request_suggestion(const std::string& question) {
    try {
        std::string payload = "{\"question\": \"" + question + "\"}";
        const char* envUrl = getenv("TASKMAN_API_URL");
        std::string url = envUrl ? std::string(envUrl) : std::string("http://127.0.0.1:8001/api/llm");
        std::string resp = http_post_json(url, payload);
        return resp;
    } catch (const std::exception& ex) {
        return std::string("{\"error\": \"") + ex.what() + "\"}";
    }
}
