#pragma once

#include <string>

namespace http_client {

//! Generate a pre-flight HTTP Negotiate authentication token.
//! Extracts the hostname from the URL to construct the SPN "HTTP@hostname",
//! then acquires a SPNEGO token via GSS-API (macOS/Linux) or SSPI (Windows).
//! Returns the base64-encoded token (without the "Negotiate " prefix).
//! Throws std::runtime_error if no security provider is available or token generation fails.
std::string GenerateNegotiateToken(const std::string &url);

//! Returns true if a security provider (GSS-API or SSPI) is available on this system.
bool NegotiateAuthIsAvailable();

} // namespace http_client
