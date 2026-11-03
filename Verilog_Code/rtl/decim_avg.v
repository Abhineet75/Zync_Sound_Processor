`timescale 1ns / 1ps
// =============================================================================
// decim_avg : boxcar-average decimator, XADC AXI-Stream in -> DMA AXI-Stream out
// =============================================================================
//
// What it does
//   * Accepts raw XADC samples (~1 MSPS) on an AXI-Stream slave port.
//   * Keeps only the samples whose TID matches CHANNEL (the XADC tags every
//     sample with the channel it came from).
//   * Adds up R = 2^LOG2R consecutive samples, then emits the top 16 bits of
//     the sum as ONE output sample. With LOG2R = 10 that is 1024 -> 1, so
//     ~961.5 kSPS in becomes ~939 S/s out.
//   * Raises TLAST on every BLOCK-th output (512 by default). The AXI DMA
//     ends a transfer on TLAST, so one DMA transfer = one 512-sample block.
//   * Counts output samples it had to throw away because the downstream FIFO
//     was full (drop_count). This must stay at 0 for the 10-minute test.
//   * Toggles blk_toggle every time a TLAST beat leaves the module. Its period
//     on the scope = 512 / (true decimated sample rate), measured in hardware.
//
// Arithmetic (why there is no divider)
//   One XADC code is 12-bit unsigned (0..4095). Summing 2^LOG2R of them needs
//   12 + LOG2R bits (22 bits for LOG2R = 10), and it can never overflow:
//   1024 * 4095 = 4,193,280 < 2^22 = 4,194,304.
//   The true average is sum / 1024 = sum >> 10, which would throw away the
//   extra precision averaging gives us. Instead we keep bits [21:6], i.e.
//   sum >> 6 = average * 16: a 16-bit value with 4 more bits than the ADC.
//   Dividing by a power of two is just choosing which wires to keep.
//
// AXI-Stream rules this module follows
//   * Input:  s_axis_tready is tied to 1. The XADC cannot be paused, so we
//             always accept; there is no backpressure on the ADC side.
//   * Output: once m_axis_tvalid is 1, m_axis_tdata / m_axis_tlast stay
//             constant until the beat is accepted (tvalid && tready).
//             If a new average is ready while the old one is still waiting,
//             the NEW one is dropped and drop_count increments. Dropped
//             samples do not advance the block counter, so every packet the
//             DMA sees is still exactly BLOCK samples long.
//
// enable
//   enable = 0 is a soft reset: accumulator, counters, drop_count and the
//   output register are cleared and incoming samples are ignored. Software
//   arms the DMA first, then sets enable = 1, so the first block starts
//   cleanly at sample 0 and nothing is dropped while the DMA is not ready.
//   Read drop_count BEFORE clearing enable.
//
// Parameters
//   LOG2R   : log2 of the decimation ratio. 10 for the real design; use a
//             small value (e.g. 2) in testbenches so simulations stay short.
//   BLOCK   : outputs per packet (TLAST period). 512 for the real design.
//   CHANNEL : XADC channel address to keep. VAUX1 = 0x11 = 17 (PYNQ-Z2 A0).
//   USE_TID : 1 = filter on TID, 0 = accept every sample (e.g. single-source
//             test pattern without a meaningful TID).
// =============================================================================

module decim_avg #(
    parameter integer LOG2R   = 10,
    parameter integer BLOCK   = 512,
    parameter integer CHANNEL = 17,
    parameter integer USE_TID = 1
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axis:m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire        aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,
    input  wire        enable,          // from AXI GPIO; 0 = soft reset

    // ---- AXI-Stream slave: from XADC Wizard (or the source mux) -------------
    input  wire [15:0] s_axis_tdata,    // 12-bit code, MSB-aligned in [15:4]
    input  wire [4:0]  s_axis_tid,      // XADC channel address of this sample
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,

    // ---- AXI-Stream master: to AXI-Stream FIFO -> AXI DMA S2MM -------------
    output reg  [15:0] m_axis_tdata,
    output wire [1:0]  m_axis_tkeep,    // both bytes always valid
    output reg         m_axis_tvalid,
    output reg         m_axis_tlast,
    input  wire        m_axis_tready,

    // ---- Status / debug ----------------------------------------------------
    output reg  [31:0] drop_count,      // to an AXI GPIO input channel
    output reg         blk_toggle       // to a Pmod pin for the scope
);

    // =========================================================================
    // Widths
    // =========================================================================
    localparam integer ACC_W = 12 + LOG2R;                      // 22 for R = 1024
    localparam integer BLK_W = (BLOCK > 1) ? $clog2(BLOCK) : 1;  // 9 for 512

    // =========================================================================
    // Input side
    // =========================================================================
    assign s_axis_tready = 1'b1;   // never stall the ADC
    assign m_axis_tkeep  = 2'b11;

    wire [11:0] code = s_axis_tdata[15:4];

    // A sample counts only if it is valid, we are enabled, and it is our channel
    wire sample_ok = s_axis_tvalid && enable &&
                     ((USE_TID == 0) || (s_axis_tid == CHANNEL));

    // =========================================================================
    // Accumulator
    // =========================================================================
    reg  [ACC_W-1:0] acc;
    reg  [LOG2R-1:0] cnt;          // which sample of the current group (0..R-1)
    reg  [BLK_W-1:0] blk;          // which output of the current block (0..BLOCK-1)

    wire [ACC_W-1:0] acc_next = acc + code;
    wire             last_in  = &cnt;          // this is the R-th sample

    // "Keep the top 16 bits" of the sum. Padding with zeros on the right makes
    // this also work when ACC_W < 16 (small LOG2R in simulation).
    wire [ACC_W+15:0] acc_wide = {acc_next, 16'd0};
    wire [15:0]       avg_out  = acc_wide[ACC_W+15 -: 16];  // = acc_next[21:6] for LOG2R=10

    // The output register can take a new value this cycle if it is empty, or
    // if its current value is being accepted at this same clock edge.
    wire out_free = !m_axis_tvalid || m_axis_tready;
    wire out_fire =  m_axis_tvalid && m_axis_tready;      // handshake this cycle

    // =========================================================================
    // Main process
    // =========================================================================
    always @(posedge aclk) begin
        if (!aresetn || !enable) begin
            acc           <= {ACC_W{1'b0}};
            cnt           <= {LOG2R{1'b0}};
            blk           <= {BLK_W{1'b0}};
            m_axis_tdata  <= 16'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            drop_count    <= 32'd0;
            blk_toggle    <= 1'b0;
        end else begin
            // ---- Output handshake: clear the slot once the beat is taken ----
            if (out_fire) begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tlast)
                    blk_toggle <= ~blk_toggle;   // one edge per finished block
            end

            // ---- Accumulate --------------------------------------------------
            if (sample_ok) begin
                cnt <= cnt + 1'b1;               // wraps naturally at R

                if (last_in) begin
                    acc <= {ACC_W{1'b0}};        // start the next group
                    if (out_free) begin
                        m_axis_tdata  <= avg_out;
                        m_axis_tvalid <= 1'b1;   // overrides the clear above
                        m_axis_tlast  <= (blk == BLOCK - 1);
                        blk           <= (blk == BLOCK - 1) ? {BLK_W{1'b0}} : blk + 1'b1;
                    end else begin
                        drop_count    <= drop_count + 1'b1;
                    end
                end else begin
                    acc <= acc_next;
                end
            end
        end
    end

endmodule
