#include <stdio.h>
#include "pico/stdlib.h"
#include "buddy2/adc.h"
#include "buddy2/pwm.h"
#include "buddy3/protocol_analyzer.h"
#include "buddy4/swd.h"
#include "buddy5/wifi_dashboard.h"

static void gpio_callback(uint gpio, uint32_t events);
static DashboardData dashboard_data = {0};


// Unified GPIO callback
static void gpio_callback(uint gpio, uint32_t events) {
    uint32_t now = time_us_32();
    static uint32_t last_button_time = 0;
    // PWM Signal (GP7)
    if (gpio == PWM_PIN && is_capturing()) {
        handle_pwm_edge(gpio, events, now);
    }
    
    // Protocol Analysis Signals
    if ((gpio == UART_RX_PIN) && is_protocol_capturing()) {
        handle_protocol_edge(gpio, events, now);
    }
}

int main() {
    stdio_init_all();
    sleep_ms(2000); // Give time for USB serial to initialize
    
    printf("\nIntegrated Signal Analyzer Program\n");
    printf("================================\n");

    // Initialize WiFi first
    if (!init_wifi_dashboard()) {
        printf("Failed to initialize WiFi dashboard. System will continue without network connectivity.\n");
    }

    // Initialize all GPIO interrupts with unified callback
    gpio_set_irq_enabled_with_callback(PWM_PIN, 
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &gpio_callback);

    // Initialize all modules
    adc_analyzer_init();
    pwm_analyzer_init();
    protocol_analyzer_init();

    // Wait for WiFi connection before showing menu
    int wifi_wait_count = 0;
    while (!is_wifi_connected() && wifi_wait_count < 30) { // Wait up to 30 seconds
        printf("Waiting for WiFi connection... %d/30\n", wifi_wait_count + 1);
        sleep_ms(1000);
        handle_dashboard_events();
        wifi_wait_count++;
    }

    if (is_wifi_connected()) {
        printf("\nWiFi connected successfully!\n");
    } else {
        printf("\nWiFi connection timed out. Continuing without network connectivity.\n");
    }


    // Main loop
    while (1) {
        if (is_wifi_connected()) {
            // Update metrics only if respective capture is active
            if (is_capturing()) {
                PWMMetrics pwm = get_pwm_metrics();
                dashboard_data.pwm_frequency = pwm.frequency;
                dashboard_data.pwm_duty_cycle = pwm.duty_cycle;
                dashboard_data.pwm_active = true;
            } else {
                if (dashboard_data.pwm_active) {  // Only reset values if we're transitioning from active to inactive
                    dashboard_data.pwm_frequency = 0;
                    dashboard_data.pwm_duty_cycle = 0;
                    dashboard_data.pwm_active = false;
                }
            }
            
            
            if (is_adc_capturing()) {
                if (is_transfer_complete()) {
                    clear_transfer_complete();
                    float freq = analyze_current_capture();
                    dashboard_data.analog_frequency = freq;
                }
                dashboard_data.adc_active = true;
            } else {
                dashboard_data.adc_active = false;
                dashboard_data.analog_frequency = 0;
            }
            
            if (is_protocol_capturing()) {
                float baud_rate = get_uart_baud_rate();
                if (baud_rate > 0) {
                    dashboard_data.uart_baud_rate = baud_rate;
                    dashboard_data.protocol_active = true;  // Make sure to update active state
                }
            }
            
            // Always update dashboard to reflect current state
            update_dashboard_data(&dashboard_data);
        }
        
        handle_dashboard_events();
        sleep_ms(100);
    }
    return 0;
}