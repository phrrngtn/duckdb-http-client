#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "http_config.hpp"
#include "lru_pool.hpp"
#include "negotiate_auth.hpp"
#include "rate_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

/* ══════════════════════════════════════════════════════════════════════
 * Global state: session pool and rate limiter registry (per-process)
 * ══════════════════════════════════════════════════════════════════════ */

using namespace blobhttp;

static LRUPool<std::string, cpr::Session> &GetSessionPool() {
	static LRUPool<std::string, cpr::Session> pool(50);
	return pool;
}

static RateLimiterRegistry &GetRateLimiterRegistry() {
	static RateLimiterRegistry registry(200);
	return registry;
}

static std::mutex g_global_limiter_mutex;
static std::unique_ptr<GCRARateLimiter> g_global_limiter;
static std::string g_global_limiter_spec;

static GCRARateLimiter *GetGlobalLimiter(const std::string &spec, double burst) {
	if (spec.empty()) return nullptr;
	std::lock_guard<std::mutex> lock(g_global_limiter_mutex);
	if (!g_global_limiter || g_global_limiter_spec != spec) {
		double rate = ParseRateLimit(spec);
		g_global_limiter = std::make_unique<GCRARateLimiter>(rate, burst, spec);
		g_global_limiter_spec = spec;
	}
	return g_global_limiter.get();
}

static GCRARateLimiter *GetGlobalLimiterSnapshot() {
	std::lock_guard<std::mutex> lock(g_global_limiter_mutex);
	return g_global_limiter.get();
}

/* ══════════════════════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════════════════════ */

static std::string ExtractHost(const std::string &url) {
	auto pos = url.find("://");
	if (pos == std::string::npos) return url;
	auto host_start = pos + 3;
	auto host_end = url.find_first_of(":/?#", host_start);
	if (host_end == std::string::npos) host_end = url.length();
	return url.substr(host_start, host_end - host_start);
}

//! Parse a JSON object string into config entries.
static std::vector<std::pair<std::string, std::string>>
ParseConfigJson(const char *str, int len) {
	std::vector<std::pair<std::string, std::string>> result;
	if (!str || len <= 0) return result;
	try {
		auto j = nlohmann::json::parse(std::string(str, len));
		if (j.is_object()) {
			for (auto &[key, val] : j.items()) {
				result.emplace_back(key, val.is_string() ? val.get<std::string>() : val.dump());
			}
		}
	} catch (...) {}
	return result;
}

//! Parse a JSON object string into header key-value pairs.
static std::vector<std::pair<std::string, std::string>>
ParseHeadersJson(const char *str, int len) {
	std::vector<std::pair<std::string, std::string>> result;
	if (!str || len <= 0) return result;
	try {
		auto j = nlohmann::json::parse(std::string(str, len));
		if (j.is_object()) {
			for (auto &[key, val] : j.items()) {
				result.emplace_back(key, val.is_string() ? val.get<std::string>() : val.dump());
			}
		}
	} catch (...) {}
	return result;
}

static void AcquireRateLimit(GCRARateLimiter *limiter) {
	if (!limiter) return;
	int max_retries = 50;
	bool was_paced = false;
	double total_pacing = 0.0;
	while (!limiter->TryAcquire() && max_retries-- > 0) {
		double wait = limiter->WaitTime();
		if (wait > 0.0) {
			was_paced = true;
			total_pacing += wait;
			std::this_thread::sleep_for(std::chrono::duration<double>(wait));
		}
	}
	limiter->RecordRequest();
	if (was_paced) limiter->RecordPacing(total_pacing);
}

static void RecordResponseStats(const cpr::Response &response, const std::string &host) {
	auto *limiter = GetRateLimiterRegistry().GetOrCreate(host);
	if (!limiter) return;
	limiter->RecordResponse(response.elapsed, response.text.size(),
	                        static_cast<int>(response.status_code));
	if (response.status_code == 429) {
		double retry_after = 1.0;
		auto it = response.header.find("Retry-After");
		if (it != response.header.end()) {
			try { retry_after = std::stod(it->second); } catch (...) {}
		}
		limiter->RecordThrottle(retry_after);
	}
	auto *global = GetGlobalLimiterSnapshot();
	if (global) {
		global->RecordResponse(response.elapsed, response.text.size(),
		                       static_cast<int>(response.status_code));
	}
}

//! Build a cpr::Session and execute a single HTTP request. Returns JSON string.
static std::string ExecuteRequest(const std::string &method, const std::string &url,
                                  const std::vector<std::pair<std::string, std::string>> &headers,
                                  const std::vector<std::pair<std::string, std::string>> &params,
                                  const std::string &body, const std::string &content_type,
                                  const HttpConfig &config) {
	auto session = std::make_shared<cpr::Session>();
	session->SetUrl(cpr::Url{url});
	session->SetTimeout(cpr::Timeout{config.timeout * 1000});

	cpr::Header cpr_headers;
	for (auto &[k, v] : headers) {
		cpr_headers[k] = v;
	}

	// Apply auth
	if (config.auth_type == "negotiate" && cpr_headers.find("Authorization") == cpr_headers.end()) {
		auto neg_result = GenerateNegotiateToken(url);
		cpr_headers["Authorization"] = "Negotiate " + neg_result.token;
	} else if (config.auth_type == "bearer" && !config.bearer_token.empty() &&
	           cpr_headers.find("Authorization") == cpr_headers.end()) {
		if (config.bearer_token_expires_at > 0) {
			auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
			    std::chrono::system_clock::now().time_since_epoch()).count();
			if (now_epoch >= config.bearer_token_expires_at) {
				throw std::runtime_error("Bearer token expired");
			}
		}
		cpr_headers["Authorization"] = "Bearer " + config.bearer_token;
	}

	auto effective_ct = content_type;
	if (!body.empty() && effective_ct.empty()) effective_ct = "application/json";
	if (!effective_ct.empty()) cpr_headers["Content-Type"] = effective_ct;

	session->SetHeader(cpr_headers);

	if (!config.verify_ssl) session->SetVerifySsl(cpr::VerifySsl{false});
	if (!config.ca_bundle.empty() || !config.client_cert.empty() || !config.client_key.empty()) {
		cpr::SslOptions ssl_opts;
		if (!config.ca_bundle.empty()) ssl_opts.SetOption(cpr::ssl::CaInfo{config.ca_bundle});
		if (!config.client_cert.empty()) ssl_opts.SetOption(cpr::ssl::CertFile{config.client_cert});
		if (!config.client_key.empty()) ssl_opts.SetOption(cpr::ssl::KeyFile{config.client_key});
		session->SetSslOptions(ssl_opts);
	}
	if (!config.proxy.empty()) {
		session->SetProxies(cpr::Proxies{{"http", config.proxy}, {"https", config.proxy}});
	}
	if (!params.empty()) {
		cpr::Parameters cpr_params;
		for (auto &[k, v] : params) {
			cpr_params.Add(cpr::Parameter{k, v});
		}
		session->SetParameters(cpr_params);
	}
	if (!body.empty()) session->SetBody(cpr::Body{body});

	// Rate limiting
	auto host = ExtractHost(url);
	AcquireRateLimit(GetGlobalLimiter(config.global_rate_limit_spec, config.global_burst));
	AcquireRateLimit(GetRateLimiterRegistry().GetOrCreate(host, config.rate_limit_spec, config.burst));

	// Execute
	cpr::Response response;
	if (method == "GET") response = session->Get();
	else if (method == "POST") response = session->Post();
	else if (method == "PUT") response = session->Put();
	else if (method == "DELETE") response = session->Delete();
	else if (method == "PATCH") response = session->Patch();
	else if (method == "HEAD") response = session->Head();
	else if (method == "OPTIONS") response = session->Options();
	else throw std::runtime_error("Unsupported HTTP method: " + method);

	RecordResponseStats(response, host);

	// Build JSON response
	nlohmann::json result;
	result["request_url"] = url;
	result["request_method"] = method;

	nlohmann::json req_hdrs = nlohmann::json::object();
	for (auto &[k, v] : cpr_headers) req_hdrs[k] = v;
	result["request_headers"] = req_hdrs;
	result["request_body"] = body;

	result["response_status_code"] = static_cast<int>(response.status_code);
	result["response_status"] = response.status_line;

	nlohmann::json resp_hdrs = nlohmann::json::object();
	for (auto &[k, v] : response.header) {
		std::string lower_k = k;
		std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(), ::tolower);
		resp_hdrs[lower_k] = v;
	}
	result["response_headers"] = resp_hdrs;
	result["response_body"] = response.text;
	result["response_url"] = response.url.str();
	result["elapsed"] = response.elapsed;
	result["redirect_count"] = static_cast<int>(response.redirect_count);

	return result.dump();
}

/* ══════════════════════════════════════════════════════════════════════
 * Scalar: bhttp_request(method, url [, headers_json [, params_json
 *                        [, body [, content_type [, config_json]]]]])
 * Returns JSON string with full request/response envelope.
 * All optional args are JSON strings — uniform with DuckDB interface.
 * ══════════════════════════════════════════════════════════════════════ */

static void bhttp_request_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	if (argc < 2) {
		sqlite3_result_error(ctx, "bhttp_request requires at least 2 arguments: method, url", -1);
		return;
	}

	const char *method_str = reinterpret_cast<const char *>(sqlite3_value_text(argv[0]));
	const char *url_str = reinterpret_cast<const char *>(sqlite3_value_text(argv[1]));
	if (!method_str || !url_str) {
		sqlite3_result_error(ctx, "method and url must not be NULL", -1);
		return;
	}

	std::string method(method_str);
	for (auto &c : method) c = toupper(c);
	std::string url(url_str);

	std::vector<std::pair<std::string, std::string>> headers;
	std::vector<std::pair<std::string, std::string>> params;
	std::string body;
	std::string content_type;
	std::vector<std::pair<std::string, std::string>> config_entries;

	if (argc >= 3 && sqlite3_value_type(argv[2]) != SQLITE_NULL) {
		headers = ParseHeadersJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[2])),
		    sqlite3_value_bytes(argv[2]));
	}
	if (argc >= 4 && sqlite3_value_type(argv[3]) != SQLITE_NULL) {
		params = ParseHeadersJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[3])),
		    sqlite3_value_bytes(argv[3]));
	}
	if (argc >= 5 && sqlite3_value_type(argv[4]) != SQLITE_NULL) {
		body = std::string(reinterpret_cast<const char *>(sqlite3_value_text(argv[4])),
		                   sqlite3_value_bytes(argv[4]));
	}
	if (argc >= 6 && sqlite3_value_type(argv[5]) != SQLITE_NULL) {
		content_type = std::string(reinterpret_cast<const char *>(sqlite3_value_text(argv[5])),
		                           sqlite3_value_bytes(argv[5]));
	}
	if (argc >= 7 && sqlite3_value_type(argv[6]) != SQLITE_NULL) {
		config_entries = ParseConfigJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[6])),
		    sqlite3_value_bytes(argv[6]));
	}

	HttpConfig config = ResolveConfig(url, config_entries);
	ResolveVaultSecrets(config, params);

	try {
		auto result = ExecuteRequest(method, url, headers, params, body, content_type, config);
		sqlite3_result_text(ctx, result.c_str(), result.length(), SQLITE_TRANSIENT);
	} catch (const std::exception &e) {
		sqlite3_result_error(ctx, e.what(), -1);
	}
}

/* ══════════════════════════════════════════════════════════════════════
 * Convenience: bhttp_get(url [, headers_json [, params_json [, config_json]]])
 * ══════════════════════════════════════════════════════════════════════ */

static void bhttp_get_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	if (argc < 1) {
		sqlite3_result_error(ctx, "bhttp_get requires at least 1 argument: url", -1);
		return;
	}

	const char *url_str = reinterpret_cast<const char *>(sqlite3_value_text(argv[0]));
	if (!url_str) { sqlite3_result_null(ctx); return; }

	std::vector<std::pair<std::string, std::string>> headers;
	std::vector<std::pair<std::string, std::string>> params;
	std::vector<std::pair<std::string, std::string>> config_entries;

	if (argc >= 2 && sqlite3_value_type(argv[1]) != SQLITE_NULL) {
		headers = ParseHeadersJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[1])),
		    sqlite3_value_bytes(argv[1]));
	}
	if (argc >= 3 && sqlite3_value_type(argv[2]) != SQLITE_NULL) {
		params = ParseHeadersJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[2])),
		    sqlite3_value_bytes(argv[2]));
	}
	if (argc >= 4 && sqlite3_value_type(argv[3]) != SQLITE_NULL) {
		config_entries = ParseConfigJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[3])),
		    sqlite3_value_bytes(argv[3]));
	}

	HttpConfig config = ResolveConfig(url_str, config_entries);
	ResolveVaultSecrets(config, params);

	try {
		auto result = ExecuteRequest("GET", url_str, headers, params, "", "", config);
		sqlite3_result_text(ctx, result.c_str(), result.length(), SQLITE_TRANSIENT);
	} catch (const std::exception &e) {
		sqlite3_result_error(ctx, e.what(), -1);
	}
}

/* ══════════════════════════════════════════════════════════════════════
 * Convenience: bhttp_post(url [, body [, headers_json [, params_json [, config_json]]]])
 * ══════════════════════════════════════════════════════════════════════ */

static void bhttp_post_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	if (argc < 1) {
		sqlite3_result_error(ctx, "bhttp_post requires at least 1 argument: url", -1);
		return;
	}

	const char *url_str = reinterpret_cast<const char *>(sqlite3_value_text(argv[0]));
	if (!url_str) { sqlite3_result_null(ctx); return; }

	std::string body;
	std::vector<std::pair<std::string, std::string>> headers;
	std::vector<std::pair<std::string, std::string>> params;
	std::vector<std::pair<std::string, std::string>> config_entries;

	if (argc >= 2 && sqlite3_value_type(argv[1]) != SQLITE_NULL) {
		body = std::string(reinterpret_cast<const char *>(sqlite3_value_text(argv[1])),
		                   sqlite3_value_bytes(argv[1]));
	}
	if (argc >= 3 && sqlite3_value_type(argv[2]) != SQLITE_NULL) {
		headers = ParseHeadersJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[2])),
		    sqlite3_value_bytes(argv[2]));
	}
	if (argc >= 4 && sqlite3_value_type(argv[3]) != SQLITE_NULL) {
		params = ParseHeadersJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[3])),
		    sqlite3_value_bytes(argv[3]));
	}
	if (argc >= 5 && sqlite3_value_type(argv[4]) != SQLITE_NULL) {
		config_entries = ParseConfigJson(
		    reinterpret_cast<const char *>(sqlite3_value_text(argv[4])),
		    sqlite3_value_bytes(argv[4]));
	}

	HttpConfig config = ResolveConfig(url_str, config_entries);
	ResolveVaultSecrets(config, params);

	try {
		auto result = ExecuteRequest("POST", url_str, headers, params, body, "", config);
		sqlite3_result_text(ctx, result.c_str(), result.length(), SQLITE_TRANSIENT);
	} catch (const std::exception &e) {
		sqlite3_result_error(ctx, e.what(), -1);
	}
}

/* ══════════════════════════════════════════════════════════════════════
 * negotiate_auth_header(url) -> TEXT
 * negotiate_auth_header_json(url) -> JSON TEXT
 * ══════════════════════════════════════════════════════════════════════ */

static void negotiate_auth_header_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *url = reinterpret_cast<const char *>(sqlite3_value_text(argv[0]));
	if (!url) { sqlite3_result_null(ctx); return; }

	try {
		auto result = GenerateNegotiateToken(url);
		std::string header = "Negotiate " + result.token;
		sqlite3_result_text(ctx, header.c_str(), header.length(), SQLITE_TRANSIENT);
	} catch (const std::exception &e) {
		sqlite3_result_error(ctx, e.what(), -1);
	}
}

static void negotiate_auth_header_json_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *url = reinterpret_cast<const char *>(sqlite3_value_text(argv[0]));
	if (!url) { sqlite3_result_null(ctx); return; }

	try {
		auto result = GenerateNegotiateToken(url);
		nlohmann::json j;
		j["token"] = result.token;
		j["header"] = "Negotiate " + result.token;
		j["url"] = result.url;
		j["hostname"] = result.hostname;
		j["spn"] = result.spn;
		j["provider"] = result.provider;
		j["library"] = result.library;
		auto json_str = j.dump();
		sqlite3_result_text(ctx, json_str.c_str(), json_str.length(), SQLITE_TRANSIENT);
	} catch (const std::exception &e) {
		sqlite3_result_error(ctx, e.what(), -1);
	}
}

/* ══════════════════════════════════════════════════════════════════════
 * bhttp_rate_limit_stats() -> JSON array
 * ══════════════════════════════════════════════════════════════════════ */

static void bhttp_rate_limit_stats_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	nlohmann::json arr = nlohmann::json::array();

	auto snapshot = [](const std::string &host, GCRARateLimiter &limiter) -> nlohmann::json {
		return {
		    {"host", host},
		    {"rate_limit", limiter.RateSpec()},
		    {"rate_rps", limiter.Rate()},
		    {"burst", limiter.Burst()},
		    {"requests", limiter.Requests()},
		    {"paced", limiter.Paced()},
		    {"total_wait_seconds", limiter.TotalWaitSeconds()},
		    {"throttled_429", limiter.Throttled429()},
		    {"backlog_seconds", limiter.BacklogSeconds()},
		    {"total_responses", limiter.TotalResponses()},
		    {"total_response_bytes", limiter.TotalResponseBytes()},
		    {"total_elapsed", limiter.TotalElapsed()},
		    {"min_elapsed", limiter.MinElapsed()},
		    {"max_elapsed", limiter.MaxElapsed()},
		    {"errors", limiter.Errors()},
		};
	};

	auto *global = GetGlobalLimiterSnapshot();
	if (global) arr.push_back(snapshot("(global)", *global));

	GetRateLimiterRegistry().ForEach([&](const std::string &host, GCRARateLimiter &limiter) {
		arr.push_back(snapshot(host, limiter));
	});

	auto result = arr.dump();
	sqlite3_result_text(ctx, result.c_str(), result.length(), SQLITE_TRANSIENT);
}

/* ══════════════════════════════════════════════════════════════════════
 * Extension entry point
 * ══════════════════════════════════════════════════════════════════════ */

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int sqlite3_bhttp_init(sqlite3 *db, char **pzErrMsg,
                       const sqlite3_api_routines *pApi) {
	SQLITE_EXTENSION_INIT2(pApi);
	int rc;

	rc = sqlite3_create_function(db, "bhttp_request", -1, SQLITE_UTF8, nullptr,
	                              bhttp_request_func, nullptr, nullptr);
	if (rc != SQLITE_OK) return rc;

	rc = sqlite3_create_function(db, "bhttp_get", -1, SQLITE_UTF8, nullptr,
	                              bhttp_get_func, nullptr, nullptr);
	if (rc != SQLITE_OK) return rc;

	rc = sqlite3_create_function(db, "bhttp_post", -1, SQLITE_UTF8, nullptr,
	                              bhttp_post_func, nullptr, nullptr);
	if (rc != SQLITE_OK) return rc;

	rc = sqlite3_create_function(db, "negotiate_auth_header", 1, SQLITE_UTF8, nullptr,
	                              negotiate_auth_header_func, nullptr, nullptr);
	if (rc != SQLITE_OK) return rc;

	rc = sqlite3_create_function(db, "negotiate_auth_header_json", 1, SQLITE_UTF8, nullptr,
	                              negotiate_auth_header_json_func, nullptr, nullptr);
	if (rc != SQLITE_OK) return rc;

	rc = sqlite3_create_function(db, "bhttp_rate_limit_stats", 0, SQLITE_UTF8, nullptr,
	                              bhttp_rate_limit_stats_func, nullptr, nullptr);
	return rc;
}
}
