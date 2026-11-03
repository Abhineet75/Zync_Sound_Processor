`timescale 1ns / 1ps
// =============================================================================
// pwm_dac : PWM "DAC" — a duty-cycle register that an RC filter turns into a voltage
// =============================================================================
//
// What it does
//   A WIDTH-bit counter runs freely at the clock rate. The output is high
//   while counter < duty, so in every period of 2^WIDTH clocks the pin is
//   high for exactly `duty` clocks:
//
//       average voltage = 3.3 V * duty / 2^WIDTH
//
//   At 100 MHz with WIDTH = 8: period = 256 clocks = 2.56 us -> ~390.6 kHz
//   carrier. A 1 kOhm + 1 uF RC low-pass (fc = 1/(2*pi*RC) ~ 159 Hz)
//   removes the carrier and leaves the slowly varying average, which is the
//   signal we want to see on the scope (5-100 Hz content passes).
//
// Glitch-free updates
//   `duty` comes from an AXI GPIO register and can change at any clock.
//   If it changed mid-period, that one period would have a wrong width.
//   So `duty` is copied into duty_q only at the end of a period
//   (counter == all ones), and the comparison always uses duty_q.
//
// Range
//   duty = 0              -> pin always low  (0 V)
//   duty = 2^WIDTH - 1    -> high 255/256 of the time (~3.287 V); 100 % is
//                            not reachable, which is normal for this design.
//
// Clock domain
//   `duty` must come from the same clock as `clk` (the AXI GPIO is clocked by
//   FCLK_CLK0, same as this module), so no synchroniser is needed.
// =============================================================================

module pwm_dac #(
    parameter integer WIDTH = 8
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_RESET resetn" *)
    input  wire             clk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 resetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire             resetn,
    input  wire [WIDTH-1:0] duty,      // from AXI GPIO
    output reg              pwm_out    // to a Pmod pin -> RC filter
);

    reg [WIDTH-1:0] cnt;
    reg [WIDTH-1:0] duty_q;

    always @(posedge clk) begin
        if (!resetn) begin
            cnt     <= {WIDTH{1'b0}};
            duty_q  <= {WIDTH{1'b0}};
            pwm_out <= 1'b0;
        end else begin
            cnt <= cnt + 1'b1;
            if (&cnt)                    // last clock of the period
                duty_q <= duty;          // new duty takes effect from cnt = 0
            pwm_out <= (cnt < duty_q);   // registered: no combinational glitches on the pin
        end
    end

endmodule
