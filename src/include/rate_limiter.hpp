#pragma once

#include "lru_pool.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

namespace http_client {

//! GCRA (Generic Cell Rate Algorithm) rate limiter with diagnostic counters
//! and adaptive 429 backoff.
//! Tracks a single timestamp (the Theoretical Arrival Time) per key.
//! No background threads, no token counters — just one time_point and arithmetic.
class GCRARateLimiter {
public:
	//! Construct a rate limiter.
	//! @param rate      requests per second
	//! @param burst     maximum burst size (requests that can arrive at once)
	//! @param rate_spec original rate spec string for diagnostics
	GCRARateLimiter(double rate, double burst, const std::string &rate_spec = "")
	    : interval_(1.0 / rate), burst_offset_(interval_ * burst), rate_(rate), burst_(burst),
	      rate_spec_(rate_spec), tat_(std::chrono::steady_clock::now()) {
	}

	//! Try to acquire permission for one request.
	//! Returns true if allowed, false if rate limit exceeded.
	bool TryAcquire() {
		auto now = std::chrono::steady_clock::now();
		// If TAT is in the past, reset to now (we haven't been using our allowance)
		auto new_tat = (tat_ < now) ? now : tat_;
		// How far into the future would the new TAT be?
		auto diff = std::chrono::duration<double>(new_tat - now).count() + interval_;
		if (diff > burst_offset_) {
			return false;
		}
		tat_ = std::chrono::time_point_cast<std::chrono::steady_clock::duration>(
		    new_tat + std::chrono::duration<double>(interval_));
		return true;
	}

	//! Returns how long the caller should wait before retrying (seconds).
	//! Returns 0.0 if a request would be allowed right now.
	double WaitTime() const {
		auto now = std::chrono::steady_clock::now();
		auto wait = std::chrono::duration<double>(tat_ - now).count();
		return (wait > 0.0) ? wait : 0.0;
	}

	//! Record that a request was made (called after rate limit check, before HTTP call).
	void RecordRequest() {
		requests_++;
	}

	//! Record that pacing was required (the caller had to sleep).
	//! @param wait_seconds how long the caller slept
	void RecordPacing(double wait_seconds) {
		paced_++;
		total_wait_seconds_ += wait_seconds;
	}

	//! Record a 429 response and back off the TAT.
	//! @param retry_after seconds to back off (from Retry-After header, or a default)
	void RecordThrottle(double retry_after) {
		throttled_429_++;
		// Push the TAT forward so subsequent requests are delayed
		auto now = std::chrono::steady_clock::now();
		auto new_tat = (tat_ < now) ? now : tat_;
		tat_ = std::chrono::time_point_cast<std::chrono::steady_clock::duration>(
		    new_tat + std::chrono::duration<double>(retry_after));
	}

	// --- Diagnostic accessors ---
	uint64_t Requests() const { return requests_; }
	uint64_t Paced() const { return paced_; }
	double TotalWaitSeconds() const { return total_wait_seconds_; }
	uint64_t Throttled429() const { return throttled_429_; }
	double Rate() const { return rate_; }
	double Burst() const { return burst_; }
	const std::string &RateSpec() const { return rate_spec_; }

	//! How far ahead the TAT is from now (seconds). Positive = backlogged.
	double BacklogSeconds() const {
		auto now = std::chrono::steady_clock::now();
		auto backlog = std::chrono::duration<double>(tat_ - now).count();
		return (backlog > 0.0) ? backlog : 0.0;
	}

private:
	double interval_;     // seconds between requests (1/rate)
	double burst_offset_; // max burst window in seconds (interval * burst)
	double rate_;         // requests per second (for diagnostics)
	double burst_;        // burst capacity (for diagnostics)
	std::string rate_spec_; // original spec string (for diagnostics)
	std::chrono::steady_clock::time_point tat_; // theoretical arrival time

	// Diagnostic counters
	uint64_t requests_ = 0;
	uint64_t paced_ = 0;
	double total_wait_seconds_ = 0.0;
	uint64_t throttled_429_ = 0;
};

//! Parse a rate limit string like "10/s", "100/m", "1000/h" into requests-per-second.
//! Returns 0.0 if the string is empty (meaning no rate limit).
inline double ParseRateLimit(const std::string &spec) {
	if (spec.empty()) {
		return 0.0;
	}

	auto slash = spec.find('/');
	if (slash == std::string::npos || slash == 0 || slash == spec.length() - 1) {
		throw std::runtime_error("Invalid rate_limit format: '" + spec + "'. Expected format: '10/s', '100/m', or '1000/h'");
	}

	double count;
	try {
		count = std::stod(spec.substr(0, slash));
	} catch (...) {
		throw std::runtime_error("Invalid rate_limit count in: '" + spec + "'");
	}

	auto unit = spec.substr(slash + 1);
	double divisor;
	if (unit == "s" || unit == "sec") {
		divisor = 1.0;
	} else if (unit == "m" || unit == "min") {
		divisor = 60.0;
	} else if (unit == "h" || unit == "hr") {
		divisor = 3600.0;
	} else {
		throw std::runtime_error("Invalid rate_limit unit '" + unit + "' in: '" + spec + "'. Use s, m, or h.");
	}

	return count / divisor;
}

//! Default rate limit applied when no scoped secret overrides it.
//! Prevents accidental server hammering from unbounded queries.
static constexpr const char *DEFAULT_RATE_LIMIT = "20/s";
static constexpr double DEFAULT_BURST = 5.0;

//! Thread-safe registry of per-host rate limiters backed by an LRU pool.
//! If no secret provides a rate_limit for a host, the session-wide default (20/s) applies.
class RateLimiterRegistry {
public:
	explicit RateLimiterRegistry(size_t max_hosts = 200) : pool_(max_hosts) {
	}

	//! Get or create a rate limiter for the given host.
	//! @param host       hostname key
	//! @param rate_spec  rate limit string from a scoped secret, or empty to use the default
	//! @param burst      burst capacity, used only on first creation
	//! @return pointer to the rate limiter (never null — the default always applies)
	GCRARateLimiter *GetOrCreate(const std::string &host, const std::string &rate_spec = "",
	                             double burst = DEFAULT_BURST) {
		auto effective_spec = rate_spec.empty() ? std::string(DEFAULT_RATE_LIMIT) : rate_spec;
		return pool_.GetOrCreate(host, [&]() {
			double rate = ParseRateLimit(effective_spec);
			return GCRARateLimiter(rate, burst, effective_spec);
		});
	}

	//! Iterate over all active rate limiters. Callback receives (host, limiter&).
	template <typename Fn>
	void ForEach(Fn fn) {
		pool_.ForEach(fn);
	}

private:
	LRUPool<std::string, GCRARateLimiter> pool_;
};

} // namespace http_client
