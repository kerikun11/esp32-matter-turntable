/**
 * SPDX-License-Identifier: LGPL-2.1
 * © 2025 Ryotaro Onuki
 */
#pragma once

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdint>

#include "device_common/system/app_log.h"

class ServoMotor {
 public:
  ServoMotor() = default;

  // pin_pwm: PWM signal pin
  // pin_pwr: power enable pin (-1 to disable)
  // pwr_on_level: power enable active level
  bool begin(int pin_pwm, int pin_pwr = -1, bool pwr_on_level = true) {
    pin_pwr_ = pin_pwr;
    pwr_on_level_ = pwr_on_level;

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = kLedcMode;
    timer_cfg.duty_resolution = kResBits;
    timer_cfg.timer_num = kLedcTimer;
    timer_cfg.freq_hz = kFreqHz;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
      LOGE("[Servo] ledc_timer_config failed: %s", esp_err_to_name(err));
      return false;
    }
    ledc_channel_config_t channel_cfg = {};
    channel_cfg.gpio_num = pin_pwm;
    channel_cfg.speed_mode = kLedcMode;
    channel_cfg.channel = kLedcChannel;
    channel_cfg.timer_sel = kLedcTimer;
    channel_cfg.duty = 0;  // keep LOW to avoid backfeed
    err = ledc_channel_config(&channel_cfg);
    if (err != ESP_OK) {
      LOGE("[Servo] ledc_channel_config failed: %s", esp_err_to_name(err));
      return false;
    }

    if (pin_pwr_ >= 0) {
      gpio_config_t cfg = {};
      cfg.pin_bit_mask = 1ULL << pin_pwr_;
      cfg.mode = GPIO_MODE_OUTPUT;
      gpio_config(&cfg);
      setPower(false);
    }

    current_ = target_ = start_ = 90.0f;
    state_ = State::kIdle;
    initialized_ = true;
    return true;
  }

  // Power off and release (PWM stays attached but output LOW)
  void free() {
    writeDuty(0);
    setPower(false);
    state_ = State::kIdle;
  }

  // speed_dps == 0: jump -> hold 100ms -> free()
  // speed_dps  > 0: pre-hold 100ms -> smooth ease-in/out -> hold 100ms -> free()
  // speed_dps specifies the peak speed during the smooth motion.
  // When the servo power is off, it is switched on first and the motion
  // starts after the power-on delay, without blocking the caller.
  void setTargetDegree(float deg, float speed_dps = 0.0f) {
    if (!initialized_) return;
    target_ = clamp(deg, 0.0f, 180.0f);
    peak_speed_dps_ = std::fabs(speed_dps);

    const int64_t now = nowMs();
    if (pin_pwr_ >= 0 && !powered_) {
      writeDuty(0);
      setPower(true);
      state_ = State::kPowerUp;
      deadline_ms_ = now + kPowerOnDelayMs;
      return;
    }
    if (state_ == State::kPowerUp) return;  // starts when power is ready
    startMotion(now);
  }

  // Call periodically
  void handle() {
    const int64_t now = nowMs();

    switch (state_) {
      case State::kPowerUp:
        if (now >= deadline_ms_) startMotion(now);
        return;

      case State::kStartHold:
        if (now >= deadline_ms_) {
          state_ = State::kMoving;
          move_start_ms_ = now;
        }
        return;

      case State::kEndHold:
        if (now >= deadline_ms_) free();
        return;

      case State::kMoving: {
        const int64_t elapsed_ms = now - move_start_ms_;
        if (elapsed_ms >= move_duration_ms_) {
          current_ = target_;
          writeUs(degToUs(current_));
          state_ = State::kEndHold;
          deadline_ms_ = now + kHoldMs;
          return;
        }

        const float t = static_cast<float>(elapsed_ms) / move_duration_ms_;
        current_ = start_ + (target_ - start_) * smoothStep(t);
        writeUs(degToUs(current_));
        return;
      }

      case State::kIdle:
      default:
        return;
    }
  }

  float getDegree() const { return current_; }
  float getTargetDegree() const { return target_; }
  bool getPowered() const { return powered_; }
  bool isBusy() const { return state_ != State::kIdle; }

 private:
  // constants
  static constexpr uint16_t kMinUs = 500;
  static constexpr uint16_t kMaxUs = 2400 + 100;  // margin for full range
  static constexpr uint32_t kFreqHz = 50;
  static constexpr ledc_timer_bit_t kResBits = LEDC_TIMER_14_BIT;
  static constexpr ledc_mode_t kLedcMode = LEDC_LOW_SPEED_MODE;
  static constexpr ledc_timer_t kLedcTimer = LEDC_TIMER_0;
  static constexpr ledc_channel_t kLedcChannel = LEDC_CHANNEL_0;
  static constexpr int64_t kHoldMs = 100;
  static constexpr int64_t kPowerOnDelayMs = 100;
  static constexpr uint32_t kPeriodUs = 1000000UL / kFreqHz;
  static constexpr uint32_t kMaxDuty = (1UL << kResBits) - 1;
  static constexpr float kSmoothStepPeakSlope = 1.5f;

  enum class State : uint8_t {
    kIdle,
    kPowerUp,
    kStartHold,
    kMoving,
    kEndHold
  };

  // pins / power
  bool initialized_ = false;
  int pin_pwr_ = -1;
  bool pwr_on_level_ = true;
  bool powered_ = false;

  // motion
  float current_ = 90.0f, target_ = 90.0f, start_ = 90.0f;
  float peak_speed_dps_ = 0.0f;
  int64_t move_start_ms_ = 0;
  int64_t move_duration_ms_ = 0;
  int64_t deadline_ms_ = 0;
  State state_ = State::kIdle;

  static int64_t nowMs() { return esp_timer_get_time() / 1000; }
  static float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }
  static uint16_t degToUs(float deg) {
    const float t = clamp(deg, 0.0f, 180.0f) / 180.0f;
    return static_cast<uint16_t>(std::lround(kMinUs + t * (kMaxUs - kMinUs)));
  }
  static float smoothStep(float t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  void startMotion(int64_t now) {
    if (peak_speed_dps_ == 0.0f) {
      current_ = target_;
      writeUs(degToUs(current_));
      state_ = State::kEndHold;
    } else {
      // pre-hold at current position
      start_ = current_;
      const float distance = std::fabs(target_ - start_);
      move_duration_ms_ = static_cast<int64_t>(std::fmax(
          1.0f, distance * kSmoothStepPeakSlope / peak_speed_dps_ * 1000.0f));
      writeUs(degToUs(current_));
      state_ = State::kStartHold;
    }
    deadline_ms_ = now + kHoldMs;
  }

  void writeUs(uint16_t us) {
    uint32_t duty = static_cast<uint32_t>(static_cast<uint64_t>(us) * kMaxDuty / kPeriodUs);
    if (duty > kMaxDuty) duty = kMaxDuty;
    writeDuty(duty);
  }

  void writeDuty(uint32_t duty) {
    if (!initialized_) return;
    ledc_set_duty(kLedcMode, kLedcChannel, duty);
    ledc_update_duty(kLedcMode, kLedcChannel);
  }

  void setPower(bool on) {
    if (pin_pwr_ < 0) return;
    gpio_set_level(static_cast<gpio_num_t>(pin_pwr_), on == pwr_on_level_ ? 1 : 0);
    powered_ = on;
  }
};
