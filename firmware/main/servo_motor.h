/**
 * SPDX-License-Identifier: LGPL-2.1
 * © 2025 Ryotaro Onuki
 */
#pragma once

#include <Arduino.h>
#include <math.h>
#include <stdint.h>

class ServoMotor {
 public:
  ServoMotor() = default;

  // pin_pwm: PWM signal pin
  // pin_pwr: power enable pin (-1 to disable)
  // pwr_on_level: power enable active level
  bool begin(int pin_pwm, int pin_pwr = -1, bool pwr_on_level = true) {
    pin_pwm_ = pin_pwm;
    pin_pwr_ = pin_pwr;
    pwr_on_level_ = pwr_on_level;

    if (!ledcAttach((uint8_t)pin_pwm_, kFreqHz, kResBits)) return false;
    ledcWrite((uint8_t)pin_pwm_, 0);  // keep LOW to avoid backfeed

    if (pin_pwr_ >= 0) {
      pinMode(pin_pwr_, OUTPUT);
      digitalWrite(pin_pwr_, pwr_on_level_ ? LOW : HIGH);  // power OFF
      powered_ = false;
    }

    current_ = target_ = start_ = 90.0f;
    move_start_ms_ = 0;
    move_duration_ms_ = 0;
    hold_until_ms_ = 0;
    state_ = State::Idle;
    return true;
  }

  // Power off and release (PWM stays attached but output LOW)
  void free() {
    ledcWrite((uint8_t)pin_pwm_, 0);
    if (pin_pwr_ >= 0) {
      digitalWrite(pin_pwr_, pwr_on_level_ ? LOW : HIGH);
      powered_ = false;
    }
    state_ = State::Idle;
    hold_until_ms_ = 0;
  }

  // speed_dps == 0: jump → hold 100ms → free()
  // speed_dps  > 0: pre-hold 100ms -> smooth ease-in/out -> hold 100ms -> free()
  // speed_dps specifies the peak speed during the smooth motion.
  void setTargetDegree(float deg, float speed_dps = 0.0f) {
    target_ = clamp_(deg, 0.0f, 180.0f);
    float peak_speed_dps = fabsf(speed_dps);
    ensurePowerOn_();

    const uint32_t now = millis();
    if (peak_speed_dps == 0.0f) {
      current_ = target_;
      writeUs_(degToUs_(current_));
      state_ = State::EndHold;
      hold_until_ms_ = now + kHoldMs;
    } else {
      // pre-hold at current position
      start_ = current_;
      float distance = fabsf(target_ - start_);
      move_duration_ms_ = (uint32_t)fmaxf(
          1.0f, distance * kSmoothStepPeakSlope / peak_speed_dps * 1000.0f);
      writeUs_(degToUs_(current_));
      state_ = State::StartHold;
      hold_until_ms_ = now + kHoldMs;
    }
  }

  // Call periodically
  void handle() {
    const uint32_t now = millis();

    switch (state_) {
      case State::StartHold:
        if (passed_(now, hold_until_ms_)) {
          state_ = State::Moving;
          move_start_ms_ = now;
        }
        return;

      case State::EndHold:
        if (passed_(now, hold_until_ms_)) {
          hold_until_ms_ = 0;
          free();
        }
        return;

      case State::Moving: {
        uint32_t elapsed_ms = now - move_start_ms_;
        if (elapsed_ms >= move_duration_ms_) {
          current_ = target_;
          writeUs_(degToUs_(current_));
          state_ = State::EndHold;
          hold_until_ms_ = now + kHoldMs;
          return;
        }

        float t = (float)elapsed_ms / (float)move_duration_ms_;
        float eased = smoothStep_(t);
        current_ = start_ + (target_ - start_) * eased;
        writeUs_(degToUs_(current_));
        return;
      }

      case State::Idle:
      default:
        return;
    }
  }

  float getDegree() const { return current_; }
  bool getPowered() const { return powered_; }

 private:
  // constants
  static constexpr uint16_t kMinUs = 500;
  static constexpr uint16_t kMaxUs = 2400 + 100;  // margin for full range
  static constexpr uint32_t kFreqHz = 50;
  static constexpr uint8_t kResBits = 16;
  static constexpr uint32_t kHoldMs = 100;
  static constexpr uint32_t kPowerOnDelayMs = 100;
  static constexpr uint32_t kPeriodUs = 1000000UL / kFreqHz;
  static constexpr uint32_t kMaxDuty = (1UL << kResBits) - 1;
  static constexpr float kSmoothStepPeakSlope = 1.5f;

  // pins / power
  int pin_pwm_ = -1;
  int pin_pwr_ = -1;
  bool pwr_on_level_ = true;
  bool powered_ = false;

  // motion
  float current_ = 90.0f, target_ = 90.0f, start_ = 90.0f;
  uint32_t move_start_ms_ = 0;
  uint32_t move_duration_ms_ = 0;
  uint32_t hold_until_ms_ = 0;
  enum class State : uint8_t { Idle, StartHold, Moving, EndHold };
  State state_ = State::Idle;

  static inline float clamp_(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }
  static inline uint16_t degToUs_(float deg) {
    float t = clamp_(deg, 0.0f, 180.0f) / 180.0f;
    return (uint16_t)lroundf(kMinUs + t * (kMaxUs - kMinUs));
  }
  static inline bool passed_(uint32_t now, uint32_t deadline) {
    return deadline != 0 && (int32_t)(now - deadline) >= 0;
  }
  static inline float smoothStep_(float t) {
    t = clamp_(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  void writeUs_(uint16_t us) {
    uint32_t duty = (uint32_t)((uint64_t)us * kMaxDuty / kPeriodUs);
    if (duty > kMaxDuty) duty = kMaxDuty;
    ledcWrite((uint8_t)pin_pwm_, duty);
  }

  void ensurePowerOn_() {
    if (pin_pwr_ >= 0 && !powered_) {
      ledcWrite((uint8_t)pin_pwm_, 0);
      digitalWrite(pin_pwr_, pwr_on_level_ ? HIGH : LOW);
      powered_ = true;
      delay(kPowerOnDelayMs);
    }
  }
};
