
#include <algorithm>

#include "drv8301.h"
#include "odrive_main.h"
#include "motor.hpp"
#include "low_level.h"

template<typename TTimer,
        TTimer* timer,
        typename TGpio,
        TGpio* pwm_al_gpio, TGpio* pwm_bl_gpio, TGpio* pwm_cl_gpio,
        TGpio* pwm_ah_gpio, TGpio* pwm_bh_gpio, TGpio* pwm_ch_gpio,
        typename TGateDriver,
        TGateDriver* gate_driver_a,
        TGateDriver* gate_driver_b,
        TGateDriver* gate_driver_c,
        typename TCurrentSensorA, TCurrentSensorA* current_sensor_a,
        typename TCurrentSensorB, TCurrentSensorB* current_sensor_b,
        typename TCurrentSensorC, TCurrentSensorC* current_sensor_c,
        typename TThermistor,
        TThermistor* inverter_thermistor_a,
        TThermistor* inverter_thermistor_b,
        TThermistor* inverter_thermistor_c,
        typename TVoltageSensor,
        TVoltageSensor* vbus_sense>
class MotorImpl : public Motor {
public:
    MotorImpl(uint16_t period, uint16_t repetition_counter, uint16_t dead_time,
              uint8_t interrupt_priority)
          : period_(period), repetition_counter_(repetition_counter), dead_time_(dead_time),
            interrupt_priority_(interrupt_priority)
    {}

    /**
     * @brief Prepares the underlying hardware for motor control
     */
    bool init() {
        // Let's be extra sure nothing unexpected happens
        disarm();

        update_current_controller_gains();

        // Init PWM
        if (!timer->init(
                period_ /* period */,
                STM32_Timer_t::UP_DOWN /* mode */,
                0 /* prescaler */,
                repetition_counter_ /* repetition counter */
            )) {
            return false;
        }


        // Ensure that debug halting of the core doesn't leave the motor PWM running
        if (!timer->set_freeze_on_dbg(true)
            || !timer->setup_pwm(1, pwm_ah_gpio, pwm_al_gpio, true, true, period_ / 2)
            || !timer->setup_pwm(2, pwm_bh_gpio, pwm_bl_gpio, true, true, period_ / 2)
            || !timer->setup_pwm(3, pwm_ch_gpio, pwm_cl_gpio, true, true, period_ / 2)
            || !timer->set_dead_time(dead_time_)) {
            return false;
        }

        using this_type = std::remove_reference_t<decltype(*this)>;
        Subscriber<>& on_update = timer->on_update_;
        if (!on_update.set<this_type, &this_type::handle_timer_update>(*this)) {
            return false;
        }

        static const float kMargin = 0.90f;
        static const float kTripMargin = 1.0f; // Trip level is at edge of linear range of amplifer

        if (is_null(gate_driver_a) || is_null(gate_driver_b) || is_null(gate_driver_c))
            return false;

        // Init ADC
        if (!gate_driver_a->init())
            return false;
        if (!gate_driver_b->init())
            return false;
        if (!gate_driver_c->init())
            return false;

        size_t n_current_sensors = 0;
        n_current_sensors += not_null(current_sensor_a) ? 1 : 0;
        n_current_sensors += not_null(current_sensor_b) ? 1 : 0;
        n_current_sensors += not_null(current_sensor_c) ? 1 : 0;
        if (n_current_sensors < 2)
            return false;

        if (not_null(current_sensor_a) && !current_sensor_a->init(config_.requested_current_range / kMargin))
            return false;
        if (not_null(current_sensor_b) && !current_sensor_b->init(config_.requested_current_range / kMargin))
            return false;
        if (not_null(current_sensor_c) && !current_sensor_c->init(config_.requested_current_range / kMargin))
            return false;

        float range_a = INFINITY, range_b = INFINITY, range_c = INFINITY;
        if (not_null(current_sensor_a) && !current_sensor_a->get_range(&range_a))
            return false;
        if (not_null(current_sensor_b) && !current_sensor_b->get_range(&range_b))
            return false;
        if (not_null(current_sensor_c) && !current_sensor_c->get_range(&range_c))
            return false;
        //enable_online_calib = current_sensor_a && current_sensor_b && current_sensor_c;
        float min_range = std::min(std::min(range_a, range_b), range_c);

        // Set trip level
        current_control_.overcurrent_trip_level.phA = (kTripMargin / kMargin) * range_a;
        current_control_.overcurrent_trip_level.phB = (kTripMargin / kMargin) * range_b;
        current_control_.overcurrent_trip_level.phC = (kTripMargin / kMargin) * range_c;

        // Clip all current control to actual usable range
        current_control_.max_allowed_current = min_range;

        if (not_null(inverter_thermistor_a) && !inverter_thermistor_a->init())
            return false;
        if (not_null(inverter_thermistor_b) && !inverter_thermistor_b->init())
            return false;
        if (not_null(inverter_thermistor_c) && !inverter_thermistor_c->init())
            return false;

        return true;
    }

    /**
     * @brief Starts the timer updates.
     */
    bool start_updates() {
        // Enable update events of all non-NULL current sensors
        if (not_null(current_sensor_a) && !current_sensor_a->enable_updates())
            return false;
        if (not_null(current_sensor_b) && !current_sensor_b->enable_updates())
            return false;
        if (not_null(current_sensor_c) && !current_sensor_c->enable_updates())
            return false;

        // Let's be extra sure nothing unexpected happens
        disarm();

        uint16_t half_timings[] = {
            (uint16_t)(period_ / 2),
            (uint16_t)(period_ / 2),
            (uint16_t)(period_ / 2)
        };
        apply_motor_pwm_timings(period_, half_timings, true);

        // Enable the update interrupt (used to coherently sample GPIO)
        if (!timer->enable_update_interrupt(interrupt_priority_))
            return false;

        if (!timer->enable_pwm(true, true, true, true, true, true, false, false))
            return false;

        if (!timer->start())
            return false;

        return true;
    }

    bool arm_foc() {
        return arm(
            [](Motor& motor, void* ctx, float pwm_timings[3]){
                (void) ctx;
                MotorImpl* asd = dynamic_cast<MotorImpl*>(&motor);
                return asd && asd->FOC(pwm_timings);
            }, nullptr);
    }

    /**
     * @brief Arms the PWM outputs that belong to this motor.
     *
     * Note that this does not yet activate the PWM outputs, it just unlocks them.
     * The PWMs are only really enabled once apply_motor_pwm_timings() is called.
     *
     * @param control_law: A function that is called at the frequency of current
     *        measurements. The function must return as quickly as possible
     *        such that the resulting PWM timings are available before the next
     *        timer update event.
     * @returns: True on success, false otherwise
     */
    bool arm(control_law_t control_law, void* ctx) {
        uint32_t mask = cpu_enter_critical();
        control_law_ctx_ = ctx;
        control_law_ = control_law;
        cpu_exit_critical(mask);

        // Reset controller states, integrators, setpoints, etc.
        axis_->controller_.reset();
        reset_current_control();

        gate_driver_a->set_enabled(true);
        gate_driver_b->set_enabled(true);
        gate_driver_c->set_enabled(true);

        mask = cpu_enter_critical();
        if (!brake_resistor_enabled || brake_resistor_armed) {
            is_armed_ = true;
        }
        cpu_exit_critical(mask);
        return true;
    }

    /**
     * @brief Disarms the motor PWM.
     * 
     * After calling this function, it is guaranteed that all three
     * motor phases are floating and will not be enabled again until
     * arm() is called.
     */
    bool disarm(bool* was_armed = nullptr) {
        uint32_t mask = cpu_enter_critical();
        if (was_armed)
            *was_armed = is_armed_;
        if (is_armed_) {
            gate_driver_a->set_enabled(false);
            gate_driver_b->set_enabled(false);
            gate_driver_c->set_enabled(false);
        }
        is_armed_ = false;
        auto instance = timer->htim.Instance;
        instance->BDTR &= ~TIM_BDTR_AOE; // prevent the PWMs from automatically enabling at the next update
        __HAL_TIM_MOE_DISABLE_UNCONDITIONALLY(&timer->htim);
        did_refresh_pwm_timings_ = false;
        control_law_ = nullptr;
        control_law_ctx_ = nullptr;
        cpu_exit_critical(mask);

        return true;
    }

    STM32_Timer_t* get_timer() { return timer; }

    void reset_current_control() {
        current_control_.v_current_control_integral_d = 0.0f;
        current_control_.v_current_control_integral_q = 0.0f;
    }

    /**
     * @brief Tune the current controller based on phase resistance and inductance
     * This should be invoked whenever one of these values changes.
     */
    void update_current_controller_gains() {
        // Calculate current control gains
        current_control_.p_gain = config_.current_control_bandwidth * config_.phase_inductance;
        float plant_pole = config_.phase_resistance / config_.phase_inductance;
        current_control_.i_gain = plant_pole * current_control_.p_gain;
    }

    // @brief Checks if the gate driver is in operational state.
    // @returns: true if the gate driver is OK (no fault), false otherwise
    bool check_DRV_fault() {
        if (!gate_driver_a->check_fault())
            return false;
        if (!gate_driver_b->check_fault())
            return false;
        if (!gate_driver_c->check_fault())
            return false;
        return true;
    }

    void set_error(Error_t error) {
        error_ |= error;
        axis_->error_ |= Axis::ERROR_MOTOR_FAILED;
        disarm();
        update_brake_current();
    }

    bool update_thermal_limits() {
        float fet_temp = std::max(std::max(inv_temp_a_, inv_temp_b_), inv_temp_c_);
        max_inv_temp_ = std::max(fet_temp, max_inv_temp_);
        float temp_margin = config_.inverter_temp_limit_upper - fet_temp;
        float derating_range = config_.inverter_temp_limit_upper - config_.inverter_temp_limit_lower;
        thermal_current_lim_ = config_.current_lim * (temp_margin / derating_range);
        if (!(thermal_current_lim_ >= 0.0f)) { //Funny polarity to also catch NaN
            thermal_current_lim_ = 0.0f;
        }
        if (fet_temp > config_.inverter_temp_limit_upper + 5) {
            set_error(ERROR_INVERTER_OVER_TEMP);
            return false;
        }
        return true;
    }

    bool do_checks() {
        if (!(vbus_voltage_ >= board_config.dc_bus_undervoltage_trip_level)) {
            set_error(ERROR_DC_BUS_UNDER_VOLTAGE);
        }
        if (!(vbus_voltage_ <= board_config.dc_bus_overvoltage_trip_level)) {
            set_error(ERROR_DC_BUS_OVER_VOLTAGE);
        }
        if (!check_DRV_fault()) {
            set_error(ERROR_DRV_FAULT);
        }
        update_thermal_limits(); // error set in function
        return error_ == ERROR_NONE;
    }

    float get_effective_current_lim() {
        // Configured limit
        float current_lim = config_.current_lim;
        // Hardware limit
        if (axis_->motor_.config_.motor_type == MOTOR_TYPE_GIMBAL) {
            current_lim = std::min(current_lim, 0.98f*one_by_sqrt3*vbus_voltage_);
        } else {
            current_lim = std::min(current_lim, axis_->motor_.current_control_.max_allowed_current);
        }
        // Thermal limit
        current_lim = std::min(current_lim, thermal_current_lim_);

        return current_lim;
    }

    void log_timing(TimingLog_t log_idx) {
        //static const uint16_t clocks_per_cnt = (uint16_t)((float)TIM_1_8_CLOCK_HZ / (float)TIM_APB1_CLOCK_HZ);
        uint16_t timing = timer->htim.Instance->CNT;
        bool counting_down = timer->htim.Instance->CR1 & TIM_CR1_DIR;
        if (counting_down != counting_down_)
            timing = 0xffff;

        if (log_idx < TIMING_LOG_NUM_SLOTS) {
            timing_log_[log_idx] = timing;
        }
    }

    //--------------------------------
    // Measurement and calibration
    //--------------------------------

    /**
     * @brief Enables the PWM outputs for 1s at 50% duty cycle and monitors the
     * phase current and DC voltage measurements.
     * 
     * This is useful to verify the following:
     *  - liveness of current sensors and DC voltage sensor
     *  - current measurements are close to zero on average
     *  - PWM switching does not generate excessive noise (with respect to the full
     *    dynamic range of the sensors)
     * 
     * If any of the above conditions do not hold, an error is set.
     * This test can be run with or without any load connected.
     * 
     * TODO: include temperature sensor
     * 
     * @param duration: Duration of the test in seconds
     */
    bool pwm_test(float duration) {
        Iph_ABC_t I_ph_min = {INFINITY, INFINITY, INFINITY};
        Iph_ABC_t I_ph_max = {-INFINITY, -INFINITY, -INFINITY};
        float vbus_voltage_avg = vbus_voltage_;
        float vbus_voltage_min = INFINITY;
        float vbus_voltage_max = -INFINITY;

        if (!arm_foc())
            return false;
        FOC_update(0.0f, 0.0f, 0.0f, 0.0f, (uint32_t)(duration * 1.0e6f) + 1000, true);

        uint32_t start = get_ticks_ms();
        while (get_ticks_ms() - start < duration * 1000.f) {
            I_ph_min.phA = std::min(I_ph_min.phA, current_meas_.phA);
            I_ph_min.phB = std::min(I_ph_min.phB, current_meas_.phB);
            I_ph_min.phC = std::min(I_ph_min.phC, current_meas_.phC);
            I_ph_max.phA = std::max(I_ph_max.phA, current_meas_.phA);
            I_ph_max.phB = std::max(I_ph_max.phB, current_meas_.phB);
            I_ph_max.phC = std::max(I_ph_max.phC, current_meas_.phC);
            vbus_voltage_min = std::min(vbus_voltage_min, vbus_voltage_);
            vbus_voltage_max = std::max(vbus_voltage_max, vbus_voltage_);
            osDelay(1);
        }

        if (axis_->error_ != Axis::ERROR_NONE)
            return false;

        bool is_current_sensor_live = (I_ph_min.phA < 0.0f && I_ph_max.phA > 0.0f)
                                && (I_ph_min.phB < 0.0f && I_ph_max.phB > 0.0f)
                                && (I_ph_min.phC < 0.0f && I_ph_max.phC > 0.0f);
        if (!is_current_sensor_live)
            return set_error(ERROR_CURRENT_SENSOR_DEAD), false;

        bool is_vbus_sensor_live = (vbus_voltage_min < vbus_voltage_avg && vbus_voltage_max > vbus_voltage_avg);
        if (!is_vbus_sensor_live)
            return set_error(ERROR_V_BUS_SENSOR_DEAD), false;

        float range_a = INFINITY, range_b = INFINITY, range_c = INFINITY;
        if (not_null(current_sensor_a) && !current_sensor_a->get_range(&range_a))
            return false;
        if (not_null(current_sensor_b) && !current_sensor_b->get_range(&range_b))
            return false;
        if (not_null(current_sensor_c) && !current_sensor_c->get_range(&range_c))
            return false;

        float vbus_voltage_range_min;
        float vbus_voltage_range_max;
        if (is_null(vbus_sense) || !vbus_sense->get_range(&vbus_voltage_range_min, &vbus_voltage_range_max))
            return false;

        bool is_noise_within_range = ((I_ph_max.phA - I_ph_min.phA) < current_control_.overcurrent_trip_level.phA * 0.05f)
                                && ((I_ph_max.phB - I_ph_min.phB) < current_control_.overcurrent_trip_level.phB * 0.05f)
                                && ((I_ph_max.phC - I_ph_min.phC) < current_control_.overcurrent_trip_level.phC * 0.05f)
                                && ((vbus_voltage_max - vbus_voltage_min) < (vbus_voltage_range_max - vbus_voltage_range_min) * 0.05f);

        if (!is_noise_within_range)
            return set_error(ERROR_TOO_NOISY), false;

        return true;
    }


    // TODO check Ibeta balance to verify good motor connection
    bool measure_phase_resistance(float test_current, float max_voltage) {
        static const float kI = 10.0f;                                 // [(V/s)/A]
        
        struct control_law_ctx_t {
            size_t i = 0;
            float test_voltage = 0.0f;
            float test_current;
            float max_voltage;
        } ctx;
        ctx.test_current = test_current;
        ctx.max_voltage = max_voltage;

        bool did_arm = arm([](Motor& motor, void* vctx, float pwm_timings[3]) {
            control_law_ctx_t& ctx = *(control_law_ctx_t*)vctx;

            float Ialpha = motor.I_alpha_beta_measured_[0];
            ctx.test_voltage += (kI * current_meas_period) * (ctx.test_current - Ialpha);
            if (ctx.test_voltage > ctx.max_voltage || ctx.test_voltage < -ctx.max_voltage)
                return motor.set_error(ERROR_PHASE_RESISTANCE_OUT_OF_RANGE), false;

            // Test voltage along phase A
            return SVM_voltage(motor.vbus_voltage_, ctx.test_voltage, 0.0f, pwm_timings);
        }, &ctx);

        if (!did_arm) {
            return set_error(ERROR_FAILED_TO_ARM), false;
        }

        osDelay(3.0f); // Test runs for 3s

        if (axis_->error_ != Axis::ERROR_NONE)
            return false;

        //// De-energize motor
        //if (!enqueue_voltage_timings(motor, 0.0f, 0.0f))
        //    return false; // error set inside enqueue_voltage_timings

        float R = ctx.test_voltage / ctx.test_current;
        config_.phase_resistance = R;
        return true; // if we ran to completion that means success
    }

    bool measure_phase_inductance(float voltage_low, float voltage_high) {
        struct control_law_ctx_t {
            float test_voltages[2];
            float Ialphas[2] = {0.0f, 0.0f};
            size_t t = 0;
        } ctx = { .test_voltages = {voltage_low, voltage_high} };

        bool did_arm = arm([](Motor& motor, void* vctx, float pwm_timings[3]) {
            control_law_ctx_t& ctx = *(control_law_ctx_t*)vctx;

            int i = ctx.t & 1;
            ctx.Ialphas[i] += motor.I_alpha_beta_measured_[0];

            // Test voltage along phase A
            return SVM_voltage(motor.vbus_voltage_, ctx.test_voltages[i], 0.0f, pwm_timings);
        }, &ctx);
        
        if (!did_arm) {
            return set_error(ERROR_FAILED_TO_ARM), false;
        }

        osDelay(5000);

        //// De-energize motor
        //if (!enqueue_voltage_timings(motor, 0.0f, 0.0f))
        //    return false; // error set inside enqueue_voltage_timings

        float v_L = 0.5f * (voltage_high - voltage_low);
        // Note: A more correct formula would also take into account that there is a finite timestep.
        // However, the discretisation in the current control loop inverts the same discrepancy
        float dI_by_dt = (ctx.Ialphas[1] - ctx.Ialphas[0]) / (current_meas_period * (float)ctx.t);
        float L = v_L / dI_by_dt;

        config_.phase_inductance = L;
        // TODO arbitrary values set for now
        if (L < 2e-6f || L > 4000e-6f)
            return set_error(ERROR_PHASE_INDUCTANCE_OUT_OF_RANGE), false;
        return true;
    }


    bool run_calibration() {
        float R_calib_max_voltage = config_.resistance_calib_max_voltage;
        if (config_.motor_type == MOTOR_TYPE_HIGH_CURRENT) {
            if (!measure_phase_resistance(config_.calibration_current, R_calib_max_voltage))
                return false;
            if (!measure_phase_inductance(-R_calib_max_voltage, R_calib_max_voltage))
                return false;
            update_current_controller_gains();
            is_calibrated_ = true;
        } else if (config_.motor_type == MOTOR_TYPE_GIMBAL) {
            // no calibration needed
        } else {
            return false;
        }
        
        return true;
    }


    bool FOC(float pwm_timings[3]) {
        float mod_to_V = (2.0f / 3.0f) * vbus_voltage_;
        float V_to_mod = 1.0f / mod_to_V;

        // Syntactic sugar
        CurrentControl_t& ictrl = current_control_;

        if (get_ticks_us() > ictrl.cmd_expiry) {
            set_error(ERROR_CONTROL_DEADLINE_MISSED);
            return false;
        }

        float I_phase = ictrl.phase;
        float phase_vel = ictrl.phase_vel;
        ictrl.phase = wrap_pm_pi(ictrl.phase + current_meas_period * phase_vel);
        float pwm_phase = I_phase + 1.5f * current_meas_period * phase_vel;
        float Id_des = ictrl.Id_setpoint;
        float Iq_des = ictrl.Iq_setpoint;

        // Clarke transform
        float Ialpha = I_alpha_beta_measured_[0];
        float Ibeta = I_alpha_beta_measured_[1];

        // Park transform
        float c_I = our_arm_cos_f32(I_phase);
        float s_I = our_arm_sin_f32(I_phase);
        float Id = c_I * Ialpha + s_I * Ibeta;
        float Iq = c_I * Ibeta - s_I * Ialpha;
        ictrl.Id_measured += config_.I_measured_report_filter_k * (Id - ictrl.Id_measured);
        ictrl.Iq_measured += config_.I_measured_report_filter_k * (Iq - ictrl.Iq_measured);
        Id = ictrl.Id_measured;
        Iq = ictrl.Iq_measured;

        // Current error
        float Ierr_d = Id_des - Id;
        float Ierr_q = Iq_des - Iq;

        if (ictrl.enable_current_control) {
            // Check for current sense saturation
            if (current_sense_saturation_) {
                set_error(ERROR_CURRENT_SENSE_SATURATION);
                return false;
            }
            if (!is_calibrated_) { // if not calibrated, p_gain and i_gain are not configured
                set_error(ERROR_NOT_CALIBRATED);
                return false;
            }

            // TODO look into feed forward terms (esp omega, since PI pole maps to RL tau)
            // Apply PI control
            ictrl.Vd_setpoint = ictrl.v_current_control_integral_d + Ierr_d * ictrl.p_gain;
            ictrl.Vq_setpoint = ictrl.v_current_control_integral_q + Ierr_q * ictrl.p_gain;
        }


        float mod_d = V_to_mod * ictrl.Vd_setpoint;
        float mod_q = V_to_mod * ictrl.Vq_setpoint;
        bool is_mod_saturated = false;

        // Vector modulation saturation, lock integrator if saturated
        // TODO make maximum modulation configurable
        float mod_scalefactor = MAX_MODULATION * sqrt3_by_2 * 1.0f / sqrtf(mod_d * mod_d + mod_q * mod_q);
        mod_scalefactor = std::min(mod_scalefactor, 1.0f);
        if (mod_scalefactor < 1.0f) {
            mod_d *= mod_scalefactor;
            mod_q *= mod_scalefactor;
            is_mod_saturated = true;
        }

        // Estimate bus current and apply limit
        float I_bus = mod_d * Id + mod_q * Iq;
        (void) I_bus;

        /* TODO: apply I_bus limit
        
        float I_bus_delta = 0.0f;

        if (I_bus > ictrl.Ibus_max_soft) {
            I_bus_delta = ictrl.Ibus_max_soft - I_bus;
            is_mod_saturated = true;
        }
        if (I_bus < ictrl.Ibus_min_soft) {
            I_bus_delta = ictrl.Ibus_min_soft - I_bus;
            is_mod_saturated = true;
        }*/

        if (ictrl.enable_current_control) {
            // Voltage saturation
            if (is_mod_saturated) {
                // TODO make decayfactor configurable
                ictrl.v_current_control_integral_d *= 0.99f;
                ictrl.v_current_control_integral_q *= 0.99f;
            } else {
                ictrl.v_current_control_integral_d += Ierr_d * (ictrl.i_gain * current_meas_period);
                ictrl.v_current_control_integral_q += Ierr_q * (ictrl.i_gain * current_meas_period);
            }
        }

        // Inverse park transform
        float c_p = our_arm_cos_f32(pwm_phase);
        float s_p = our_arm_sin_f32(pwm_phase);
        float mod_alpha = c_p * mod_d - s_p * mod_q;
        float mod_beta  = c_p * mod_q + s_p * mod_d;

        // Report final applied voltage in rotating frame
        ictrl.final_v_d = mod_to_V * mod_d;
        ictrl.final_v_q = mod_to_V * mod_q;

        // Report final applied voltage in stationary frame (for sensorles estimator)
        ictrl.final_v_alpha = mod_to_V * mod_alpha;
        ictrl.final_v_beta = mod_to_V * mod_beta;

        // Apply SVM
        if (SVM(mod_alpha, mod_beta, &pwm_timings[0], &pwm_timings[1], &pwm_timings[2]) != 0)
            return set_error(ERROR_MODULATION_MAGNITUDE), false;
        //log_timing(TIMING_LOG_FOC_CURRENT);
        return true;
    }


    bool FOC_update(float Id_setpoint, float Iq_setpoint, float phase, float phase_vel, uint32_t expiry_us, bool force_voltage_control) {
        bool enable_current_control;
        if (config_.motor_type == MOTOR_TYPE_GIMBAL || force_voltage_control) {
            enable_current_control = false;
        } else if (config_.motor_type == MOTOR_TYPE_HIGH_CURRENT) {
            enable_current_control = true;
        } else {
            set_error(ERROR_NOT_IMPLEMENTED_MOTOR_TYPE);
            return false;
        }
        if (error_ != ERROR_NONE) {
            return false;
        }

        uint32_t mask = cpu_enter_critical();
        current_control_.enable_current_control = enable_current_control;
        if (enable_current_control) {
            current_control_.Id_setpoint = Id_setpoint;
            current_control_.Iq_setpoint = Iq_setpoint * config_.direction;
        } else {
            current_control_.Vd_setpoint = Id_setpoint;
            current_control_.Vq_setpoint = Iq_setpoint * config_.direction;
        }
        current_control_.phase = phase * config_.direction + config_.phase_delay;
        current_control_.phase_vel = phase_vel * config_.direction;
        current_control_.cmd_expiry = get_ticks_us() + expiry_us; // data expires in 1ms
        cpu_exit_critical(mask);

        return true;
    }


private:


    /**
     * @brief Updates the phase PWM timings unless the motor is disarmed.
     *
     * If the motor is armed, the PWM timings come into effect at the next update
     * event (and are enabled if they weren't already), unless the motor is disarmed
     * prior to that.
     * 
     * @param tentative: If true, the update is not counted as "refresh".
     */
    void apply_motor_pwm_timings(uint16_t period, uint16_t timings[3], bool tentative) {
        uint32_t mask = cpu_enter_critical();
        if (brake_resistor_enabled && !brake_resistor_armed) {
            set_error(ERROR_BRAKE_RESISTOR_DISARMED);
        }

        timer->htim.Instance->CCR1 = timings[0];
        timer->htim.Instance->CCR2 = timings[1];
        timer->htim.Instance->CCR3 = timings[2];
        timer->htim.Instance->ARR = period;
        
        if (!tentative) {
            did_refresh_pwm_timings_ = true;
            if (is_armed_) {
                // Set the Automatic Output Enable, so that the Master Output Enable bit
                // will be automatically enabled on the next update event.
                timer->htim.Instance->BDTR |= TIM_BDTR_AOE;
            }
        }
        
        // If a timer update event occurred just now while we were updating the
        // timings, we can't be sure what values the shadow registers now contain,
        // so we must disarm the motor.
        // (this also protects against the case where the update interrupt has too
        // low priority, but that should not happen)
        if (__HAL_TIM_GET_FLAG(&timer->htim, TIM_FLAG_UPDATE)) {
            set_error(Motor::ERROR_CONTROL_DEADLINE_MISSED);
        }
        
        cpu_exit_critical(mask);
    }

    static bool check_update_mode(Motor::UpdateMode_t mode, bool counting_down) {
        switch (mode) {
            case Motor::ON_TOP: return counting_down;
            case Motor::ON_BOTTOM: return !counting_down;
            case Motor::ON_BOTH: return true;
            default: return false;
        }
    }

    /**
     * @brief Callback for the timer update event.
     * 
     * This function samples the current measurements, runs all actions that should
     * run based on them at the highest possible frequency and finally applies the
     * new PWM timings before the next update event occurs.
     * 
     * TODO: Document how the phasing is done, link to timing diagram
     */
    void handle_timer_update() {
#define calib_tau 0.2f  //@TODO make more easily configurable
        static const float calib_filter_k = current_meas_period / calib_tau;

        update_events_++; // for debugging only

        // If the corresponding timer is counting up, we just sampled in SVM vector 0, i.e. real current
        // If we are counting down, we just sampled in SVM vector 7, with zero current
        bool counting_down = timer->htim.Instance->CR1 & TIM_CR1_DIR;
        if (counting_down_ == counting_down) {
            set_error(ERROR_TIMER_UPDATE_MISSED);
            return;
        }
        counting_down_ = counting_down;

        log_timing(TIMING_LOG_UPDATE_START);

        bool should_have_updated_pwm = check_update_mode(pwm_update_mode_, counting_down_);

        // TODO: implement double rate updates, then we need to run this check also
        // while counting down
        if (should_have_updated_pwm) {
            // We only care about the PWM timings being refreshed if the output is
            // actually enabled. This gives the application time to do some tasks
            // after arming the motor but before applying the first PWM timings.
            bool is_output_enabled = timer->htim.Instance->BDTR & TIM_BDTR_MOE;
            if (is_output_enabled && !did_refresh_pwm_timings_) {
                // PWM values were not updated in time - shut down PWM
                set_error(ERROR_CONTROL_DEADLINE_MISSED);
            }
            did_refresh_pwm_timings_ = false;
        } else {
            if (!did_refresh_pwm_timings_) {
                // We're already at half the PWM period and the new timings were not
                // yet written.
                // Therefore we tentatively reset PWM values to 50%. If they are
                // overwritten before the next timer update occurs, this has no
                // effect. Otherwise, 50% duty cycle comes into effect until this
                // interrupt handler is executed again and disarms the output alltogether.
                uint16_t half_timings[] = {
                    (uint16_t)(period_ / 2),
                    (uint16_t)(period_ / 2),
                    (uint16_t)(period_ / 2)
                };
                apply_motor_pwm_timings(period_, half_timings, true);
            }
        }

        if (should_have_updated_pwm) {
            axis_->encoder_.sample_now();

            for (int i = 0; i < n_GPIO_samples; ++i) {
                GPIO_port_samples[i] = GPIOs_to_samp[i]->IDR;
            }
        }

        float temp = 0.0f;
        float vbus_voltage_k = std::min(current_meas_period / config_.vbus_voltage_tau, 1.0f);
        if (config_.vbus_voltage_override > 1.0f) {
            vbus_voltage_ = config_.vbus_voltage_override;
        } else {
            if (is_null(vbus_sense) || !vbus_sense->get_voltage(&temp)) {
                set_error(ERROR_V_BUS_SENSOR_DEAD);
                return;
            }
            vbus_voltage_ += vbus_voltage_k * (temp - vbus_voltage_);
        }

        float inv_temp_k = std::min(current_meas_period / config_.inv_temp_tau, 1.0f);
        if (not_null(inverter_thermistor_a)) {
            inv_temp_a_ = inverter_thermistor_a->read_temp(&temp) ? ((abs(inv_temp_a_) < INFINITY) ? (inv_temp_a_ + inv_temp_k * (temp - inv_temp_a_)) : temp) : INFINITY;
        }
        if (not_null(inverter_thermistor_b)) {
            inv_temp_b_ = inverter_thermistor_b->read_temp(&temp) ? ((abs(inv_temp_b_) < INFINITY) ? (inv_temp_b_ + inv_temp_k * (temp - inv_temp_b_)) : temp) : INFINITY;
        }
        if (not_null(inverter_thermistor_c)) {
            inv_temp_c_ = inverter_thermistor_c->read_temp(&temp) ? ((abs(inv_temp_c_) < INFINITY) ? (inv_temp_c_ + inv_temp_k * (temp - inv_temp_c_)) : temp) : INFINITY;
        }

        // Wait for the current measurements to become available
        bool have_all_values = false;
        have_all_values = (is_null(current_sensor_a) || current_sensor_a->has_value())
                    && (is_null(current_sensor_b) || current_sensor_b->has_value())
                    && (is_null(current_sensor_c) || current_sensor_c->has_value());
        if (!have_all_values) {
            set_error(ERROR_CURRENT_SENSOR_DEAD);
            return;
        }

        Iph_ABC_t current = { 0 };
        bool read_all_values = (is_null(current_sensor_a) || current_sensor_a->get_current(&current.phA))
                            && (is_null(current_sensor_b) || current_sensor_b->get_current(&current.phB))
                            && (is_null(current_sensor_c) || current_sensor_c->get_current(&current.phC));
        if (!read_all_values) {
            set_error(ERROR_CURRENT_SENSOR);
            return;
        }

        bool reset_all_values = (is_null(current_sensor_a) || current_sensor_a->reset_value())
                            && (is_null(current_sensor_b) || current_sensor_b->reset_value())
                            && (is_null(current_sensor_c) || current_sensor_c->reset_value());
        if (!reset_all_values) {
            set_error(ERROR_CURRENT_SENSOR);
            return;
        }

        // Infer the missing current value
        // This assumes that at least two current sensors are non-NULL (checked in init())
        if (is_null(current_sensor_a))
            current.phA = -(current.phB + current.phC);
        if (is_null(current_sensor_b))
            current.phB = -(current.phC + current.phA);
        if (is_null(current_sensor_c))
            current.phC = -(current.phA + current.phB);

        log_timing(TIMING_LOG_CURRENT_MEAS);    

        bool was_current_dc_calib = check_update_mode(current_dc_calib_mode_, counting_down_) || !is_armed_;
        if (was_current_dc_calib) {
            // DC_CAL measurement
            DC_calib_.phA += (current.phA - DC_calib_.phA) * calib_filter_k;
            DC_calib_.phB += (current.phB - DC_calib_.phB) * calib_filter_k;
            DC_calib_.phC += (current.phC - DC_calib_.phC) * calib_filter_k;
            log_timing(TIMING_LOG_DC_CAL);
        }

        bool was_current_sense = check_update_mode(current_sample_mode_, counting_down_);
        if (was_current_sense) {
            current.phA -= DC_calib_.phA;
            current.phB -= DC_calib_.phB;
            current.phC -= DC_calib_.phC;
            I_leak = current.phA + current.phB + current.phC; // sum should be close to 0
            current_meas_.phA = current.phA - I_leak / 3;
            current_meas_.phB = current.phB - I_leak / 3;
            current_meas_.phC = current.phC - I_leak / 3;
            if (!(abs(I_leak) < config_.max_leak_current)) {
                set_error(ERROR_LEAK_CURRENT_TOO_HIGH);
                return;
            }

            current_sense_saturation_ =
                (not_null(current_sensor_a) && (current_meas_.phA > current_control_.overcurrent_trip_level.phA)) ||
                (not_null(current_sensor_b) && (current_meas_.phB > current_control_.overcurrent_trip_level.phB)) ||
                (not_null(current_sensor_c) && (current_meas_.phC > current_control_.overcurrent_trip_level.phC));
            
            //if (enable_online_calib) {
            //    DC_calib_.phA += (I_leak / 3 - DC_calib_.phA) * calib_filter_k;
            //    DC_calib_.phB += (I_leak / 3 - DC_calib_.phB) * calib_filter_k;
            //    DC_calib_.phC += (I_leak / 3 - DC_calib_.phC) * calib_filter_k;
            //}

            // Clarke transform
            I_alpha_beta_measured_[0] = current_meas_.phA;
            I_alpha_beta_measured_[1] = one_by_sqrt3 * (current_meas_.phB - current_meas_.phC);

            // TODO: raise error if the sum of the current sensors diverge too much from 0

            // Prepare hall readings
            // TODO move this to inside encoder update function
            axis_->encoder_.decode_hall_samples(GPIO_port_samples);

            if (!do_checks()) {
                return; // error set in do_checks()
            }

            // Apply control law to calculate PWM duty cycles
            if (is_armed_) {

                float pwm_timings[3];
                bool success = false;
                // control_law_ and control_law_ctx_ are supposed to be set atomically in arm()
                if (control_law_) {
                    success = control_law_(*this, control_law_ctx_, pwm_timings);
                }

                if (!success) {
                    set_error(ERROR_CONTROLLER_FAILED);
                    return;
                }

                // Calculate DC power consumption
                float I_bus = (pwm_timings[0] - 0.5f) * current.phA + (pwm_timings[1] - 0.5f) * current.phB + (pwm_timings[2] - 0.5f) * current.phC;
                if (I_bus < config_.I_bus_hard_min || I_bus > config_.I_bus_hard_max) {
                    set_error(ERROR_I_BUS_OUT_OF_RANGE);
                    return;
                }
                I_bus_ = I_bus;

                uint16_t next_timings[] = {
                    (uint16_t)(pwm_timings[0] * (float)period_),
                    (uint16_t)(pwm_timings[1] * (float)period_),
                    (uint16_t)(pwm_timings[2] * (float)period_)
                };

                apply_motor_pwm_timings(period_, next_timings, false);
            } else {
                // if the phases are floating and we still have current it's because
                // the diodes are conducting, which implies a phase voltage of DC+ or DC-
                float pwm_timings[3] = {
                    current.phA > 0.0f ? 0.0f : 1.0f,
                    current.phB > 0.0f ? 0.0f : 1.0f,
                    current.phC > 0.0f ? 0.0f : 1.0f
                };
                float I_bus = (pwm_timings[0] - 0.5f) * current.phA + (pwm_timings[1] - 0.5f) * current.phB + (pwm_timings[2] - 0.5f) * current.phC;
                (void) I_bus; // TODO: discuss whether we want to handle overvoltage on the phases or just be passive about it
                I_bus_ = 0.0f;
            }

            update_brake_current();
            log_timing(TIMING_LOG_CTRL_DONE);
        }
    }

    uint16_t period_;
    uint16_t repetition_counter_;
    uint16_t dead_time_;
    uint8_t interrupt_priority_;

    uint16_t GPIO_port_samples[n_GPIO_samples];
};