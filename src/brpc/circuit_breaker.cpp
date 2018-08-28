// Copyright (c) 2014 Baidu, Inc.G
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Authors: Lei He (helei@qiyi.com)

#include <cmath>
#include <gflags/gflags.h>
#include "brpc/circuit_breaker.h"

namespace brpc {

DEFINE_int32(circuit_breaker_short_window_size, 100,
    "Short window sample size.");
DEFINE_int32(circuit_breaker_long_window_size, 1000,
    "Long window sample size.");
DEFINE_int32(circuit_breaker_short_window_error_percent, 5,
    "The maximum error rate allowed by the short window, ranging from 0-99.");
DEFINE_int32(circuit_breaker_long_window_error_percent, 3, 
    "The maximum error rate allowed by the long window, ranging from 0-99.");
DEFINE_int32(circuit_breaker_min_error_cost_us, 100,
    "The minimum error_cost, when the ema of error cost is less than this "
    "value, it will be set to zero.");
DEFINE_int32(circuit_breaker_max_failed_latency_mutliple, 2,
    "The maximum multiple of the latency of the failed request relative to "
    "the average latency of the success requests.");

namespace {
// EPSILON is used to generate the smoothing coefficient when calculating EMA.
// The larger the EPSILON, the larger the smoothing coefficient, which means 
// that the proportion of early data is larger.
// smooth = pow(EPSILON, 1 / window_size), 
// eg: when window_size = 100,
// EPSILON = 0.1, smooth = 0.9772
// EPSILON = 0.3, smooth = 0.9880
// when window_size = 1000,
// EPSILON = 0.1, smooth = 0.9977
// EPSILON = 0.3, smooth = 0.9987
const double EPSILON = 0.1;
}  // namepace

CircuitBreaker::EmaErrorRecorder::EmaErrorRecorder(int window_size, 
                                                   int max_error_percent)
    : _window_size(window_size)
    , _max_error_percent(max_error_percent)
    , _smooth(std::pow(EPSILON, 1.0/window_size))
    , _init_completed(false)
    , _sample_count(0)
    , _ema_error_cost(0)
    , _ema_latency(0) 
    , _broken(false) {
}

bool CircuitBreaker::EmaErrorRecorder::OnCallEnd(int error_code, 
                                                 int64_t latency) {
    if (_broken.load(butil::memory_order_relaxed)) {
        return false;
    }

    int64_t ema_latency = 0;
    bool healthy = false;
    if (error_code == 0) {
        ema_latency = UpdateLatency(latency);
        healthy = UpdateErrorCost(0, ema_latency);
    } else {
        ema_latency = _ema_latency.load(butil::memory_order_relaxed);
        healthy = UpdateErrorCost(latency, ema_latency);
    }

    int sample_count = _sample_count.fetch_add(1, butil::memory_order_relaxed);
    bool init_completed = _init_completed.load(butil::memory_order_acquire);
    if (!init_completed && sample_count >= _window_size) {
        _init_completed.store(true, butil::memory_order_release);
        init_completed = true;
    }
    
    if (!init_completed) {
        return true;
    }
    if (!healthy) {
        _broken.store(true, butil::memory_order_relaxed);
    }
    return healthy;
}

void CircuitBreaker::EmaErrorRecorder::Reset() {
    _init_completed.store(false, butil::memory_order_relaxed);
    _sample_count.store(0, butil::memory_order_relaxed);
    _ema_error_cost.store(0, butil::memory_order_relaxed);
    _ema_latency.store(0, butil::memory_order_relaxed);
    _broken.store(false, butil::memory_order_relaxed);
}

int64_t CircuitBreaker::EmaErrorRecorder::UpdateLatency(int64_t latency) {
    while (true) {
        int64_t ema_latency = _ema_latency.load(butil::memory_order_relaxed);
        int64_t next_ema_latency = 0;
        if (0 == ema_latency) {
            next_ema_latency = latency;
        } else {
            next_ema_latency = ema_latency * _smooth + latency * (1 - _smooth);
        }
        if (_ema_latency.compare_exchange_weak(ema_latency, next_ema_latency)) {
            return next_ema_latency;
        }
    }
}

bool CircuitBreaker::EmaErrorRecorder::UpdateErrorCost(int64_t error_cost, 
                                                       int64_t ema_latency) {
    const int max_mutilple = FLAGS_circuit_breaker_max_failed_latency_mutliple;
    error_cost = std::min(ema_latency * max_mutilple, error_cost);
    //Errorous response
    if (error_cost != 0) {
        int64_t ema_error_cost = 
            _ema_error_cost.fetch_add(error_cost, butil::memory_order_relaxed);
        ema_error_cost += error_cost; 
        int64_t max_error_cost = ema_latency * _window_size * 
            (_max_error_percent / 100.0) * (1.0 + EPSILON);
        return ema_error_cost <= max_error_cost;
    }

    //Ordinary response
    while (true) {
        int64_t ema_error_cost = 
            _ema_error_cost.load(butil::memory_order_relaxed);
        if (ema_error_cost == 0) {
            break;
        } else if (ema_error_cost < FLAGS_circuit_breaker_min_error_cost_us) {
            if (_ema_error_cost.compare_exchange_weak(
                ema_error_cost, 0, butil::memory_order_relaxed)) {
                break;
            }
        } else {
            int64_t next_ema_error_cost = ema_error_cost * _smooth;
            if (_ema_error_cost.compare_exchange_weak(
                ema_error_cost, next_ema_error_cost)) {
                break;
            }
        }
    }
    return true;
}

CircuitBreaker::CircuitBreaker()
    : _long_window(FLAGS_circuit_breaker_long_window_size,
                   FLAGS_circuit_breaker_long_window_error_percent)
    , _short_window(FLAGS_circuit_breaker_short_window_size,
                    FLAGS_circuit_breaker_short_window_error_percent) {
}

bool CircuitBreaker::OnCallEnd(int error_code, int64_t latency) {
    return _long_window.OnCallEnd(error_code, latency) && 
           _short_window.OnCallEnd(error_code, latency);
}

void CircuitBreaker::Reset() {
    _long_window.Reset();
    _short_window.Reset();
}

}  // namespace brpc
