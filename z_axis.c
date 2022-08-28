#include "z_axis.h"
#include "config/motion.h"
#include "config/tmc.h"
#include "drivers/tmc2209_helper.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include <math.h>
#include <stdio.h>

/*
    Macros and constants
*/
enum HomingState {
    HOMING_NONE = 0,
    HOMING_SEEKING = 1,
    HOMING_BOUNCING = 2,
    HOMING_RESEEKING = 3,
    HOMING_FINISHED = 4
};

/*
    Static variables
*/
volatile struct ZMotor* current_motor;

/*
    Forward declarations
*/
static void setup_move(volatile struct ZMotor* m, float dest_mm);
static bool step_timer_callback(repeating_timer_t* rt);
static void diag_pin_irq();
static void debug_stallguard(volatile struct ZMotor* m);

/*
    Public methods
*/

void ZMotor_init(
    struct ZMotor* m, struct TMC2209* tmc, uint32_t pin_enn, uint32_t pin_dir, uint32_t pin_step, uint32_t pin_diag) {
    m->tmc = tmc;
    m->pin_enn = pin_enn;
    m->pin_dir = pin_dir;
    m->pin_step = pin_step;
    m->pin_diag = pin_diag;

    m->actual_steps = 0;

    m->_step_edge = 0;
    m->_dir = 1;

    m->_accel_step_count = 0;
    m->_decel_step_count = 0;
    m->_coast_step_count = 0;
    m->_total_step_count = 0;
    m->_current_step_count = 0;

    m->_crash_flag = 0;

    m->velocity_mm_s = Z_DEFAULT_VELOCITY_MM_S;
    m->acceleration_mm_s2 = Z_DEFAULT_ACCELERATION_MM_S2;
    m->homing_sensitivity = Z_HOMING_SENSITIVITY;
}

bool ZMotor_setup(struct ZMotor* m) {
    gpio_init(m->pin_enn);
    gpio_set_dir(m->pin_enn, GPIO_OUT);
    gpio_put(m->pin_enn, true);

    gpio_init(m->pin_dir);
    gpio_set_dir(m->pin_dir, GPIO_OUT);
    gpio_put(m->pin_dir, false);

    gpio_init(m->pin_step);
    gpio_set_dir(m->pin_step, GPIO_OUT);
    gpio_put(m->pin_step, false);

    gpio_init(m->pin_diag);
    gpio_set_dir(m->pin_diag, GPIO_IN);
    gpio_pull_down(m->pin_diag);

    if (!TMC2209_write_config(m->tmc, m->pin_enn)) {
        printf("Error configuring Z motor TMC2209!");
        return false;
    }

    current_motor = m;

    printf("Configuring DIAG interrupt...\n");
    gpio_set_irq_enabled_with_callback(m->pin_diag, GPIO_IRQ_EDGE_RISE, true, &diag_pin_irq);

    printf("Starting stepper timer...\n");
    add_repeating_timer_us(1000, step_timer_callback, NULL, &(m->_step_timer));

    return true;
}

void ZMotor_home(volatile struct ZMotor* m) {
    printf("> Homing Z...\n");

    float old_velocity = m->velocity_mm_s;
    float old_acceleration = m->acceleration_mm_s2;
    m->velocity_mm_s = Z_HOMING_VELOCITY_MM_S;
    m->acceleration_mm_s2 = Z_HOMING_ACCELERATION_MM_S2;

    printf("> Enabling stallguard with threshold at %u\n", m->homing_sensitivity);
    TMC2209_write(m->tmc, TMC2209_SGTHRS, m->homing_sensitivity);

    printf("> Seeking endstop...\n");
    m->_crash_flag = false;
    m->actual_steps = 0;
    setup_move(m, Z_HOMING_DIR * Z_HOMING_DISTANCE_MM);

    while (!m->_crash_flag) {
        tight_loop_contents();
        // sleep_ms(50);
        // debug_stallguard(m);
    }

    ZMotor_stop(m);

    printf("> Endstop found, bouncing...\n");
    TMC2209_write(m->tmc, TMC2209_SGTHRS, 0);
    m->_crash_flag = false;
    ZMotor_reset_position(m);
    setup_move(current_motor, -(Z_HOMING_DIR * Z_HOMING_BOUNCE_MM));

    while (ZMotor_is_moving(m)) {
        tight_loop_contents();
    }

    printf("> Re-seeking...\n");
    setup_move(m, Z_HOMING_DIR * Z_HOMING_BOUNCE_MM * 2);

    // Ignore stallguard output until it's had some time to move.
    sleep_ms(2);

    TMC2209_write(m->tmc, TMC2209_SGTHRS, m->homing_sensitivity);
    m->_crash_flag = false;

    while (!m->_crash_flag) {
        tight_loop_contents();
        // sleep_ms(50);
        // debug_stallguard(m);
    }

    ZMotor_stop(m);

    printf("> Found! Saving home position...\n");
    ZMotor_reset_position(m);

    printf("> Disabling stallguard...\n");
    TMC2209_write(m->tmc, TMC2209_SGTHRS, 0);

    m->velocity_mm_s = old_velocity;
    m->acceleration_mm_s2 = old_acceleration;
    printf("> Homing complete!\n");
}

void ZMotor_move_to(volatile struct ZMotor* m, float dest_mm) {
    setup_move(m, dest_mm);

    // Wait for the move to complete.
    while(m->_total_step_count != 0) {
        tight_loop_contents();
    }

    printf("> Move finished at %0.2f (%i steps).\n", ZMotor_get_position_mm(m), m->actual_steps);
}


float ZMotor_get_position_mm(volatile struct ZMotor* m) {
    return (float)(m->actual_steps) * Z_MM_PER_STEP;
}

/*
    Private methods
*/

static void setup_move(volatile struct ZMotor* m, float dest_mm) {
    // Calculate how far to move to bring the motor to the destination.
    float delta_mm = dest_mm - ZMotor_get_position_mm(m);
    m->_dir = delta_mm < 0 ? -1 : 1;

    // Determine the number of steps needed to complete the move.
    float delta_mm_abs = fabs(delta_mm);
    m->_total_step_count = (int32_t)(lroundf(delta_mm_abs * (float)(Z_STEPS_PER_MM)));

    // Determine how long acceleration and deceleration will take and
    // how many steps will be spent in each of the three phases (accelerating,
    // coasting, decelerating).
    float accel_time_s = m->velocity_mm_s / m->acceleration_mm_s2;
    float accel_distance_mm = 0.5f * accel_time_s * m->velocity_mm_s;
    m->_accel_step_count = (int32_t)(lroundf(accel_distance_mm * (float)(Z_STEPS_PER_MM)));
    m->_decel_step_count = m->_accel_step_count;
    m->_coast_step_count = m->_total_step_count - m->_accel_step_count * 2;

    // Check for the case where a move is too short to reach full velocity
    // and therefore has no coasting phase. In this case, the acceleration
    // and deceleration phases will each occupy one half of the total steps.
    if(m->_coast_step_count < 0) {
        m->_accel_step_count = m->_total_step_count / 2;
        // Note: use subtraction here instead of just setting it the same
        // as the acceleration step count. This accommodates odd amounts of
        // total steps and ensures that the correct amount of total steps
        // are taken. For example, if there are 11 total steps then
        // _accel_step_count = 5 and _decel_step_count = 6.
        m->_decel_step_count = m->_total_step_count - m->_accel_step_count;
        m->_coast_step_count = 0;
    }

    // Calculate the *actual* distance that the motor will move based on the
    // stepping resolution.
    float actual_delta_mm = m->_dir * (m->_total_step_count) * Z_MM_PER_STEP;
    printf("> Moving Z %0.3f mm (%i steps)\n", actual_delta_mm, m->_dir * m->_total_step_count);

    // Kick-off the step timer.
    m->_current_step_count = 0;
    m->_step_timer.delay_us = 10;
}

static void diag_pin_irq(uint32_t pin, uint32_t events) {
    uint32_t irq_status = save_and_disable_interrupts();
    current_motor->_crash_flag = true;
    restore_interrupts(irq_status);
}

static bool step_timer_callback(repeating_timer_t* rt) {
    uint32_t irq_status = save_and_disable_interrupts();
    volatile struct ZMotor* m = current_motor;

    if(m->_total_step_count == 0) {
        goto exit;
    }

    gpio_put(m->pin_dir, m->_dir == 1 ? 1 : 0);
    gpio_put(m->pin_step, m->_step_edge);
    m->_step_edge = !m->_step_edge;

    if (m->_step_edge == false) {
        m->_current_step_count++;
        m->actual_steps += m->_dir;

        // Is the move finished?
        if(m->_current_step_count == m->_total_step_count) {
            m->_current_step_count = 0;
            m->_total_step_count = 0;
            goto exit;
        }

        // Calculate instantenous velocity at the current
        // distance traveled.
        float distance = m->_current_step_count * Z_MM_PER_STEP;
        float inst_velocity;

        // Acceleration phase
        if(m->_current_step_count < m->_accel_step_count) {
            inst_velocity = sqrtf(2.0f * distance * m->acceleration_mm_s2);
        }
        // Coast phase
        else if(m->_current_step_count < m->_accel_step_count + m->_coast_step_count){
            inst_velocity = m->velocity_mm_s;
        }
        // Deceleration phase
        else {
            float total_distance = m->_total_step_count * Z_MM_PER_STEP;
            inst_velocity = sqrtf(2.0f * (total_distance - distance) * m->acceleration_mm_s2);
        }

        // Calculate the timer period from the velocity
        float s_per_step;
        if(inst_velocity > 0.0f) {
            float steps_per_s = inst_velocity / Z_MM_PER_STEP;
            s_per_step = 1.0f / steps_per_s;
        } else {
            s_per_step = 0.001f;
        }

        int64_t step_time_us = (int64_t)(s_per_step * 1000000.0f);
        m->_step_timer.delay_us = step_time_us > 1000 ? 1000 : step_time_us;
    }

exit:
    restore_interrupts(irq_status);
    return true;
}

static void debug_stallguard(volatile struct ZMotor* m) {
    uint32_t sg_result;
    TMC2209_read(m->tmc, TMC2209_SG_RESULT, &sg_result);
    printf("> SG: %u\n", sg_result);
}
