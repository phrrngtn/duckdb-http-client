#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "http_config.hpp"
#include "lru_pool.hpp"
#include "negotiate_auth.hpp"
#include "rate_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

namespace nb = nanobind;
using namespace blobhttp;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static std::string ExtractHost(const std::string &url) {
	auto pos = url.find("://");
	if (pos == std::string::npos) return url;
	auto host_start = pos + 3;
	auto host_end = url.find_first_of(":/?#", host_start);
	if (host_end == std::string::npos) host_end = url.length();
	return url.substr(host_start, host_end - host_start);
}

static void AcquireRL(GCRARateLimiter *limiter) {
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

/* ── HttpClient class ────────────────────────────────────────────────── */

struct HttpClient {
	// Per-client state
	std::vector<std::pair<std::string, std::string>> config_entries;
	RateLimiterRegistry rate_registry{200};
	std::mutex global_limiter_mutex;
	std::unique_ptr<GCRARateLimiter> global_limiter;
	std::string global_limiter_spec;

	GCRARateLimiter *GetGlobalLimiter(const std::string &spec, double burst) {
		if (spec.empty()) return nullptr;
		std::lock_guard<std::mutex> lock(global_limiter_mutex);
		if (!global_limiter || global_limiter_spec != spec) {
			double rate = ParseRateLimit(spec);
			global_limiter = std::make_unique<GCRARateLimiter>(rate, burst, spec);
			global_limiter_spec = spec;
		}
		return global_limiter.get();
	}

	GCRARateLimiter *GetGlobalLimiterSnapshot() {
		std::lock_guard<std::mutex> lock(global_limiter_mutex);
		return global_limiter.get();
	}

	void config_set(const std::string &scope, const std::string &config_json) {
		// Remove existing entry for this scope
		config_entries.erase(
		    std::remove_if(config_entries.begin(), config_entries.end(),
		                   [&](auto &p) { return p.first == scope; }),
		    config_entries.end());
		config_entries.emplace_back(scope, config_json);
	}

	void config_remove(const std::string &scope) {
		config_entries.erase(
		    std::remove_if(config_entries.begin(), config_entries.end(),
		                   [&](auto &p) { return p.first == scope; }),
		    config_entries.end());
	}

	std::optional<std::string> config_get(const std::string &scope) {
		for (auto &[k, v] : config_entries) {
			if (k == scope) return v;
		}
		return std::nullopt;
	}

	nb::dict request(const std::string &method_in, const std::string &url,
	                 std::optional<nb::dict> headers_opt,
	                 std::optional<std::string> body_opt,
	                 std::optional<std::string> content_type_opt) {

		std::string method = method_in;
		for (auto &c : method) c = toupper(c);

		HttpConfig config = ResolveConfig(url, config_entries);

		auto session = std::make_shared<cpr::Session>();
		session->SetUrl(cpr::Url{url});
		session->SetTimeout(cpr::Timeout{config.timeout * 1000});

		cpr::Header cpr_headers;
		if (headers_opt) {
			for (auto item : *headers_opt) {
				cpr_headers[nb::cast<std::string>(item.first)] = nb::cast<std::string>(item.second);
			}
		}

		// Auth
		if (config.auth_type == "negotiate" && cpr_headers.find("Authorization") == cpr_headers.end()) {
			auto neg = GenerateNegotiateToken(url);
			cpr_headers["Authorization"] = "Negotiate " + neg.token;
		} else if (config.auth_type == "bearer" && !config.bearer_token.empty() &&
		           cpr_headers.find("Authorization") == cpr_headers.end()) {
			if (config.bearer_token_expires_at > 0) {
				auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
				    std::chrono::system_clock::now().time_since_epoch()).count();
				if (now_epoch >= config.bearer_token_expires_at) {
					throw nb::value_error("Bearer token expired");
				}
			}
			cpr_headers["Authorization"] = "Bearer " + config.bearer_token;
		}

		std::string body = body_opt.value_or("");
		std::string ct = content_type_opt.value_or("");
		if (!body.empty() && ct.empty()) ct = "application/json";
		if (!ct.empty()) cpr_headers["Content-Type"] = ct;

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
		if (!body.empty()) session->SetBody(cpr::Body{body});

		// Rate limiting
		auto host = ExtractHost(url);
		AcquireRL(GetGlobalLimiter(config.global_rate_limit_spec, config.global_burst));
		AcquireRL(rate_registry.GetOrCreate(host, config.rate_limit_spec, config.burst));

		// Execute
		cpr::Response response;
		if (method == "GET") response = session->Get();
		else if (method == "POST") response = session->Post();
		else if (method == "PUT") response = session->Put();
		else if (method == "DELETE") response = session->Delete();
		else if (method == "PATCH") response = session->Patch();
		else if (method == "HEAD") response = session->Head();
		else if (method == "OPTIONS") response = session->Options();
		else throw nb::value_error(("Unsupported HTTP method: " + method).c_str());

		// Record stats
		auto *limiter = rate_registry.GetOrCreate(host);
		if (limiter) {
			limiter->RecordResponse(response.elapsed, response.text.size(),
			                        static_cast<int>(response.status_code));
		}

		// Build result dict
		nb::dict result;
		result["request_url"] = url;
		result["request_method"] = method;

		nb::dict req_hdrs;
		for (auto &[k, v] : cpr_headers) req_hdrs[nb::str(k.c_str())] = v;
		result["request_headers"] = req_hdrs;
		result["request_body"] = body;

		result["response_status_code"] = static_cast<int>(response.status_code);
		result["response_status"] = response.status_line;

		nb::dict resp_hdrs;
		for (auto &[k, v] : response.header) {
			std::string lower_k = k;
			std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(), ::tolower);
			resp_hdrs[nb::str(lower_k.c_str())] = v;
		}
		result["response_headers"] = resp_hdrs;
		result["response_body"] = response.text;
		result["response_url"] = response.url.str();
		result["elapsed"] = response.elapsed;
		result["redirect_count"] = static_cast<int>(response.redirect_count);

		return result;
	}

	nb::dict get(const std::string &url, std::optional<nb::dict> headers = std::nullopt) {
		return request("GET", url, headers, std::nullopt, std::nullopt);
	}

	nb::dict post(const std::string &url, std::optional<std::string> body = std::nullopt,
	              std::optional<nb::dict> headers = std::nullopt,
	              std::optional<std::string> content_type = std::nullopt) {
		return request("POST", url, headers, body, content_type);
	}

	nb::list rate_limit_stats() {
		nb::list result;

		auto snapshot = [](const std::string &host, GCRARateLimiter &limiter) -> nb::dict {
			nb::dict d;
			d["host"] = host;
			d["rate_limit"] = limiter.RateSpec();
			d["rate_rps"] = limiter.Rate();
			d["burst"] = limiter.Burst();
			d["requests"] = limiter.Requests();
			d["paced"] = limiter.Paced();
			d["total_wait_seconds"] = limiter.TotalWaitSeconds();
			d["throttled_429"] = limiter.Throttled429();
			d["backlog_seconds"] = limiter.BacklogSeconds();
			d["total_responses"] = limiter.TotalResponses();
			d["total_response_bytes"] = limiter.TotalResponseBytes();
			d["total_elapsed"] = limiter.TotalElapsed();
			d["min_elapsed"] = limiter.MinElapsed();
			d["max_elapsed"] = limiter.MaxElapsed();
			d["errors"] = limiter.Errors();
			return d;
		};

		auto *global = GetGlobalLimiterSnapshot();
		if (global) result.append(snapshot("(global)", *global));

		rate_registry.ForEach([&](const std::string &host, GCRARateLimiter &limiter) {
			result.append(snapshot(host, limiter));
		});

		return result;
	}
};

/* ── Module-level negotiate functions ────────────────────────────────── */

static nb::dict negotiate_token(const std::string &url) {
	auto result = GenerateNegotiateToken(url);
	nb::dict d;
	d["token"] = result.token;
	d["header"] = "Negotiate " + result.token;
	d["url"] = result.url;
	d["hostname"] = result.hostname;
	d["spn"] = result.spn;
	d["provider"] = result.provider;
	d["library"] = result.library;
	return d;
}

static bool negotiate_available() {
	return NegotiateAuthIsAvailable();
}

/* ── module definition ──────────────────────────────────────────────── */

NB_MODULE(blobhttp_ext, m) {
	m.doc() = "blobhttp — enterprise HTTP client with rate limiting, SPNEGO auth, and connection pooling";

	nb::class_<HttpClient>(m, "HttpClient")
	    .def(nb::init<>())
	    .def("config_set", &HttpClient::config_set,
	         nb::arg("scope"), nb::arg("config_json"))
	    .def("config_remove", &HttpClient::config_remove,
	         nb::arg("scope"))
	    .def("config_get", &HttpClient::config_get,
	         nb::arg("scope"))
	    .def("request", &HttpClient::request,
	         nb::arg("method"), nb::arg("url"),
	         nb::arg("headers") = nb::none(),
	         nb::arg("body") = nb::none(),
	         nb::arg("content_type") = nb::none())
	    .def("get", &HttpClient::get,
	         nb::arg("url"), nb::arg("headers") = nb::none())
	    .def("post", &HttpClient::post,
	         nb::arg("url"), nb::arg("body") = nb::none(),
	         nb::arg("headers") = nb::none(),
	         nb::arg("content_type") = nb::none())
	    .def("rate_limit_stats", &HttpClient::rate_limit_stats);

	m.def("negotiate_token", &negotiate_token, nb::arg("url"),
	      "Generate a pre-flight SPNEGO/Negotiate token for the given URL");
	m.def("negotiate_available", &negotiate_available,
	      "Check if SPNEGO/Negotiate authentication is available on this system");
}
