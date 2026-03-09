#pragma once
#include <string>

// Sends a question to the backend LLM endpoint and returns the answer (blocking)
std::string request_suggestion(const std::string& question);
