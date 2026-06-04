#include <8052.h>

// =========================================================
// MODULE TYPE CONFIGURATION
// Select a value according to your board:
//   1 = 8 inputs (P0) and 16 outputs (P1, P2)  [Standard]
//   2 = 8 inputs (P0) and 8 outputs (P1)       [Compact]
//   3 = 16 inputs (P1, P2) and 8 outputs (P0)  [Reversed layout]
// =========================================================
#define MODULE_TYPE 1

#define DIR_PIN     P3_7  
#define WD_LED      P3_6  
#define DATA_165    P3_4

__idata unsigned char rx_buffer[32]; 
__idata unsigned char tx_buffer[32]; 

unsigned char my_modbus_address = 31; 

// =========================================================
// GLOBAL SETTINGS (Modified via Modbus)
// =========================================================
volatile unsigned char cfg_wd_ticks = 10; // Watchdog timeout (10 ticks * ~40 ms = 400 ms)
volatile unsigned char cfg_tx_delay = 10; // TX hold delay for stop bit (nop cycles)

volatile unsigned char comm_watchdog = 10; 
volatile unsigned char led_timer = 0;
volatile __bit base_led_state = 0; 

// =========================================================
// TIMER 0 HARDWARE INTERRUPT
// =========================================================
void timer0_isr(void) __interrupt(1) {
    TH0 = 0x63; 
    TL0 = 0xC0; 
    
    WD_LED = !base_led_state; 
    __asm nop __endasm; 
    WD_LED = base_led_state; 

    // --- SAFETY WATCHDOG ---
    if (comm_watchdog < cfg_wd_ticks) { 
        comm_watchdog++;
        if (comm_watchdog == cfg_wd_ticks) {
            #if MODULE_TYPE == 1
                P1 = 0xFF; P2 = 0xFF;
            #elif MODULE_TYPE == 2
                P1 = 0xFF;
            #elif MODULE_TYPE == 3
                P0 = 0xFF; 
            #endif
        }
    }

    led_timer++;
    if (comm_watchdog < cfg_wd_ticks) {
        if (led_timer >= 5) { // 5 Hz (link active)
            base_led_state = !base_led_state; WD_LED = base_led_state; led_timer = 0;
        }
    } else {
        if (led_timer >= 50) { // 0.5 Hz (link lost)
            base_led_state = !base_led_state; WD_LED = base_led_state; led_timer = 0;
        }
    }
}

unsigned int CRC16(__idata unsigned char *buf, unsigned char len) {
    unsigned int crc = 0xFFFF;
    unsigned char i, j;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1) { crc >>= 1; crc ^= 0xA001; } 
            else { crc >>= 1; }
        }
    }
    return crc;
}

// =========================================================
// READ ADDRESS FROM DIP SWITCHES
// =========================================================
unsigned char Read_HW_Config(void) {
    unsigned int raw_data = 0; 
    unsigned char i, delay;
    unsigned char temp, reversed;
    
    DATA_165 = 1; WD_LED = 0;   
    
    for (i = 0; i < 9; i++) {
        raw_data <<= 1;
        if (DATA_165) raw_data |= 1;
        WD_LED = 1; for(delay=0; delay<10; delay++) __asm nop __endasm; 
        WD_LED = 0; for(delay=0; delay<10; delay++) __asm nop __endasm; 
    }
    
    raw_data = ~raw_data;
    temp = (unsigned char)(raw_data & 0x1F); 
    
    reversed = 0;
    if (temp & 0x10) reversed |= 0x01; 
    if (temp & 0x08) reversed |= 0x02; 
    if (temp & 0x04) reversed |= 0x04; 
    if (temp & 0x02) reversed |= 0x08; 
    if (temp & 0x01) reversed |= 0x10; 
    
    return reversed; 
}

void Init_System(void) {
    DIR_PIN = 1; WD_LED = 0; 
    P0 = 0xFF; P1 = 0xFF; P2 = 0xFF; 
    T2CON = 0x34; RCAP2H = 0xFF; RCAP2L = 0xFF; TH2 = 0xFF; TL2 = 0xFF; SCON = 0x50;   
    TMOD = 0x01; TH0 = 0xFF; TL0 = 0xFF; 
    ET0 = 1; EA = 1; RI = 0; TI = 0; SBUF = 0; 
}

// =========================================================
// SEND RESPONSE (OPTIMIZED FOR 750k BAUD)
// =========================================================
void Send_Response(unsigned char len) {
    unsigned char i; unsigned int delay;
    
    DIR_PIN = 0; // Switch MAX485 to transmit mode (TX)
    for(delay = 0; delay < 1000; delay++) __asm nop __endasm; 
    
    for (i = 0; i < len; i++) { 
        TI = 0; SBUF = tx_buffer[i]; while (!TI); 
    }
    
    // Wait for stop bit to finish (uses configurable delay)
    for(delay = 0; delay < cfg_tx_delay; delay++) __asm nop __endasm; 
    
    RI = 0;      // Clear any stray bytes in the buffer first
    DIR_PIN = 1; // Then switch MAX485 back to receive mode (RX)
}

// =========================================================
// MAIN LOOP
// =========================================================
void main(void) {
    __data unsigned int rx_crc, calc_crc, tx_crc;
    __data unsigned char tx_len;
    __data unsigned int start_reg, num_regs;
    __data unsigned int timeout;
    __data unsigned char rx_index;
    __data int i;
    __data unsigned char temp_addr;

    for(i=0; i<5000; i++) __asm nop __endasm; 
    temp_addr = Read_HW_Config() & 0x1F; 
    
    if (temp_addr > 0 && temp_addr <= 31) my_modbus_address = temp_addr;
    else my_modbus_address = 31; 

    Init_System(); 
    TR0 = 1; 

    while (1) {
        if (RI) {
            unsigned char b = SBUF; RI = 0;
            if (b != my_modbus_address) continue; 
            
            rx_buffer[0] = b; rx_index = 1; timeout = 0;
            while (timeout < 500) { 
                if (RI) { rx_buffer[rx_index++] = SBUF; RI = 0; timeout = 0; if (rx_index == 8) break; } 
                else { timeout++; }
            }

            if (rx_index == 8) {
                calc_crc = CRC16(rx_buffer, 6);
                rx_crc = (rx_buffer[7] << 8) | rx_buffer[6];

                if (calc_crc == rx_crc) {
                    comm_watchdog = 0; 

                    if (rx_buffer[1] == 0x03) { 
                        start_reg = (rx_buffer[2] << 8) | rx_buffer[3];
                        num_regs = (rx_buffer[4] << 8) | rx_buffer[5];
                        tx_buffer[0] = my_modbus_address; tx_buffer[1] = 0x03; tx_buffer[2] = num_regs * 2; 

                        for (i = 0; i < num_regs; i++) {
                            // --- READ ---
                            #if MODULE_TYPE == 1
                                if (start_reg + i == 0) { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = P0; } 
                                else if (start_reg + i == 1) { tx_buffer[3 + i*2] = ~P2; tx_buffer[4 + i*2] = ~P1; } 
                            #elif MODULE_TYPE == 2
                                if (start_reg + i == 0) { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = P0; } 
                                else if (start_reg + i == 1) { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = ~P1; } 
                            #elif MODULE_TYPE == 3
                                if (start_reg + i == 0) { tx_buffer[3 + i*2] = P2; tx_buffer[4 + i*2] = P1; } 
                                else if (start_reg + i == 1) { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = ~P0; } 
                            #endif
                            else if (start_reg + i == 2) { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = my_modbus_address; }
                            else if (start_reg + i == 3) { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = MODULE_TYPE; } 
                            else { tx_buffer[3 + i*2] = 0x00; tx_buffer[4 + i*2] = 0x00; }
                        }
                        tx_len = 3 + (num_regs * 2); tx_crc = CRC16(tx_buffer, tx_len);
                        tx_buffer[tx_len++] = tx_crc & 0xFF; tx_buffer[tx_len++] = (tx_crc >> 8) & 0xFF; 
                        Send_Response(tx_len);
                    }
                    else if (rx_buffer[1] == 0x06) {
                        start_reg = (rx_buffer[2] << 8) | rx_buffer[3];
                        
                        // --- WRITE ---
                        if (start_reg == 1) { // OUTPUTS
                            #if MODULE_TYPE == 1
                                P2 = ~rx_buffer[4]; P1 = ~rx_buffer[5]; 
                            #elif MODULE_TYPE == 2
                                P1 = ~rx_buffer[5]; 
                            #elif MODULE_TYPE == 3
                                P0 = ~rx_buffer[5]; 
                            #endif
                        }
                        else if (start_reg == 2) { // WATCHDOG CONFIG
                            if (rx_buffer[5] > 0) cfg_wd_ticks = rx_buffer[5];
                        }
                        else if (start_reg == 3) { // TX PAUSE CONFIG
                            cfg_tx_delay = rx_buffer[5];
                        }
                        
                        for (i = 0; i < 6; i++) { tx_buffer[i] = rx_buffer[i]; }
                        tx_crc = CRC16(tx_buffer, 6); tx_buffer[6] = tx_crc & 0xFF; tx_buffer[7] = (tx_crc >> 8) & 0xFF; 
                        Send_Response(8);
                    }
                }
            }
        }
    }
}
