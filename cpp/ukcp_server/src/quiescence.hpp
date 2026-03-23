#pragma once

#include <chrono>
#include <cstdint>

namespace ukcp {

class QuiescenceDrain {
      public:
        explicit QuiescenceDrain(std::chrono::milliseconds quiet_period) : quiet_period_(quiet_period) {}

        void Observe(std::uint64_t progress_value, std::chrono::steady_clock::time_point now) {
                if (!started_ || progress_value != last_progress_value_) {
                        started_ = true;
                        last_progress_value_ = progress_value;
                        idle_deadline_ = now + quiet_period_;
                }
        }

        bool Done(std::chrono::steady_clock::time_point now) const noexcept { return started_ && now >= idle_deadline_; }

      private:
        std::chrono::milliseconds quiet_period_;
        bool started_{false};
        std::uint64_t last_progress_value_{0};
        std::chrono::steady_clock::time_point idle_deadline_{};
};

} // namespace ukcp
