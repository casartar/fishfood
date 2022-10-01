#pragma once

#include "drivers/tmc2209.h"
#include "pico/time.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct DoubleLinearAxis {
    char name;

    // hardware configuration
    struct TMC2209* tmc_a;
    struct TMC2209* tmc_b;
    uint32_t pin_enn_a;
    uint32_t pin_enn_b;
    uint32_t pin_dir_a;
    uint32_t pin_dir_b;
    uint32_t pin_step_a;
    uint32_t pin_step_b;
    uint32_t pin_diag_a;
    uint32_t pin_diag_b;

    // Motion configuration. These members can be changed directly.
    bool reversed;
    float steps_per_mm;

    // Maximum velocity in mm/s
    float velocity_mm_s;
    // Constant acceleration in mm/s^2
    float acceleration_mm_s2;

    // Which direction to home, either -1 for backwards or +1 for forwards.
    int8_t homing_direction;
    // How far to try to move during homing.
    float homing_distance_mm;
    // How far to move back before re-homing.
    float homing_bounce_mm;
    // Homing velocity and acceleration
    float homing_velocity_mm_s;
    float homing_acceleration_mm_s2;
    // Homing sensitivity, used to set the TMC2209's stallguard threshold.
    // Higher = more sensitive.
    uint8_t homing_sensitivity;

    // The actual position of the motor measured in steps. This can be used
    // to derive the actual position in millimeters. (read only)
    int32_t actual_steps;

    // internal stepping state

    // Note: it takes two calls to LinearAxis_step() to complete an actual motor
    // step. This is because the first call send the falling edge and the
    // second calls the rising edge.
    // Time between subsequent calls to LinearAxis_step()
    int64_t _step_interval;
    // Time when the LinearAxis_step() will actually step.
    absolute_time_t _next_step_at;

    // The state of the output pin, used to properly toggle the step output.
    bool _step_edge;
    // The direction the motor is going in (1 or -1).
    int8_t _dir;

    // internal acceleration and velocity state for the current move.

    // Total number of steps to spend accelerating.
    int32_t _accel_step_count;
    // Total number of steps to spend decelerating.
    int32_t _decel_step_count;
    // Total number of steps between accelerating and decelerating.
    int32_t _coast_step_count;
    // Total number of steps that need to be taken.
    int32_t _total_step_count;
    // Number of steps taken so far.
    int32_t _current_step_count;

    // Set whenever stallguard is triggered and causes the DIAG pin to rise.
    int8_t _crash_flag;
};

void DoubleLinearAxis_init(
    struct DoubleLinearAxis* m,
    char name,
    struct TMC2209* tmc_a,
    uint32_t pin_enn_a,
    uint32_t pin_dir_a,
    uint32_t pin_step_a,
    uint32_t pin_diag_a,
    struct TMC2209* tmc_b,
    uint32_t pin_enn_b,
    uint32_t pin_dir_b,
    uint32_t pin_step_b,
    uint32_t pin_diag_b);
bool DoubleLinearAxis_setup(struct DoubleLinearAxis* m);
void DoubleLinearAxis_home(volatile struct DoubleLinearAxis* m);
void DoubleLinearAxis_start_move(volatile struct DoubleLinearAxis* m, float dest_mm);
void DoubleLinearAxis_wait_for_move(volatile struct DoubleLinearAxis* m);
float DoubleLinearAxis_get_position_mm(volatile struct DoubleLinearAxis* m);
inline void DoubleLinearAxis_reset_position(volatile struct DoubleLinearAxis* m) {
    m->actual_steps = 0;
    m->_total_step_count = 0;
    m->_current_step_count = 0;
}
inline bool DoubleLinearAxis_is_moving(volatile struct DoubleLinearAxis* m) { return m->_total_step_count != 0; }
inline void DoubleLinearAxis_stop(volatile struct DoubleLinearAxis* m) {
    m->_total_step_count = 0;
    m->_current_step_count = 0;
}

void DoubleLinearAxis_step(volatile struct DoubleLinearAxis* m);
