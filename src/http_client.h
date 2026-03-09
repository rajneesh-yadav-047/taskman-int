#pragma once
#include <string>

// Simple HTTP POST JSON helper using libcurl (blocking)
std::string http_post_json(const std::string& url, const std::string& json);
