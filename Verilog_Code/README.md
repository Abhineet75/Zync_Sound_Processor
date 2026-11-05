# Verilog_Code — FPGA fabric for the Low-Frequency Signal Processor

This folder holds every piece of **custom** hardware (HDL you write yourself) for the project, plus the pin constraints and the settings for the Xilinx IP blocks they connect to. The AXI DMA, AXI-Stream FIFO, AXI GPIO and XADC Wizard are Xilinx IP that you configure in Vivado. This README explains how each one must be set up so it works with the modules here.

```
Verilog_Code/
├── README.md                  ← this file
├── rtl/
│   ├── decim_avg.v            ← decimator: 1024-sample average, TLAST every 512, drop counter
│   ├── pwm_dac.v              ← 8-bit PWM "DAC" for the RC filter output
│   ├── axis_test_pattern.v    ← fake XADC: known waveforms for debugging the data path
│   └── axis_src_mux.v         ← selects real XADC or test pattern
└── constraints/
    └── pynq_z2.xdc            ← pin assignments (verify against the PYNQ-Z2 master XDC)
```

All modules are plain Verilog-2001, so Vivado can use them directly as **module references** in a block design (IP integrator does not accept SystemVerilog at the top of a module reference).

Testbenches come in the next step, one per module.

---

## 1. Where these modules sit

```
              ┌──────────────────── Programmable logic (FCLK_CLK0 = 100 MHz) ───────────────────┐
  A0 ──────▶  │ XADC Wizard ──s0──┐                                                             │
 (0–3.3 V)    │  ~961.5 kSPS      │                                                             │
              │                   ▼                                                             │
              │            ┌─────────────┐    ┌───────────┐   ┌──────────────┐   ┌──────────┐   │
              │            │axis_src_mux │──▶ │ decim_avg │──▶│ AXIS Data    │──▶│ AXI DMA  │───┼─▶ HP0 ─▶ DDR
              │            └─────────────┘    │ ÷1024,    │   │ FIFO (1024)  │   │ S2MM     │   │   (ping/pong
              │                   ▲    sel    │ TLAST/512 │   └──────────────┘   └────┬─────┘   │    buffers)
              │ axis_test_pattern ┘     ▲     └─┬───┬─────┘                           │ irq     │
              │   ▲ mode, phase_inc     │       │   └── blk_toggle ──────────────────────────────┼─▶ JA7
              │   │                     │       └── drop_count ──▶ gpio_in ch1        ▼         │
              │ gpio_ctrl ──────────────┴── enable                               IRQ_F2P ▶ ARM  │
              │                                                                                 │
              │ gpio_out ch1 (duty) ──▶ pwm_dac ──▶ pwm_out ────────────────────────────────────┼─▶ JA1 ▶ RC ▶ scope
              │ gpio_out ch2 ─────────────────────▶ dbg_pins[3:0] ──────────────────────────────┼─▶ JA2–4, LED0
              │ sw[1:0] ──▶ gpio_in ch2                                                         │
              └─────────────────────────────────────────────────────────────────────────────────┘
```

The design splits work between hardware and software like this:

- **Fabric:** sampling, averaging and moving data. These happen ~1 million times per second, run at a fixed rate, and need no decisions, which suits hardware well.
- **ARM:** Goertzel filters and detection. These run once per block, about twice a second. They need multiplications, flexible thresholds and printing, which suits software well.

---

## 2. The numbers (read this before trusting "977 S/s")

The project plan assumes the XADC runs at exactly 1 MSPS, which would give 1,000,000 / 1024 ≈ 977 S/s after decimation. With the standard 100 MHz fabric clock it will be slightly lower:

| Quantity | Value | Why |
|---|---|---|
| DCLK (XADC clock) | 100 MHz | FCLK_CLK0 from the PS |
| ADCCLK | 25 MHz | DCLK ÷ 4 (ADCCLK may not exceed 26 MHz, so ÷4 is the smallest divider) |
| Raw sample rate | 25 MHz ÷ 26 = **961.5 kSPS** | One conversion takes 26 ADCCLK cycles in continuous mode |
| Decimated rate | 961,538 ÷ 1024 = **939.0 S/s** | |
| Block period | 512 ÷ 939.0 = **545 ms** | Deadline for processing one block |
| 50 % deadline target | **≈ 273 ms** | |
| Goertzel bin spacing | 939.0 ÷ 512 = **1.83 Hz** | |

The XADC Wizard's summary page shows the real conversion rate it chose. **Measure the real rate anyway** with the `blk_toggle` pin: period ÷ 512 = sample period. Use that measured `fs` in `goertzel_coeff_q14()` in software.

---

## 3. Module by module

### 3.1 `decim_avg.v` — the decimator (the most important module)

**Job:** it turns ~961,538 noisy 12-bit samples per second into ~939 cleaner 16-bit samples per second. It also cuts the stream into 512-sample packets the DMA can store.

#### Ports

| Port | Dir | Width | Connects to | Meaning |
|---|---|---|---|---|
| `aclk` | in | 1 | FCLK_CLK0 | 100 MHz clock |
| `aresetn` | in | 1 | `peripheral_aresetn` of the Processor System Reset IP | Active-low reset |
| `enable` | in | 1 | `gpio_ctrl` ch2 bit 0 | 0 = soft reset (everything cleared, input ignored) |
| `s_axis_tdata` | in | 16 | mux output | XADC code in bits [15:4] |
| `s_axis_tid` | in | 5 | mux output | XADC channel address of that sample |
| `s_axis_tvalid` | in | 1 | mux output | Sample present |
| `s_axis_tready` | out | 1 | mux output | Always 1 |
| `m_axis_tdata` | out | 16 | FIFO `S_AXIS` | Averaged sample (unsigned) |
| `m_axis_tkeep` | out | 2 | FIFO `S_AXIS` | Always `11` (both bytes valid) |
| `m_axis_tvalid` | out | 1 | FIFO `S_AXIS` | Output sample present |
| `m_axis_tlast` | out | 1 | FIFO `S_AXIS` | 1 on the 512th sample of every block |
| `m_axis_tready` | in | 1 | FIFO `S_AXIS` | FIFO can accept |
| `drop_count` | out | 32 | `gpio_in` ch1 | Outputs lost because the FIFO was full |
| `blk_toggle` | out | 1 | Pmod JA7 | Flips once per finished block |

#### How it works, step by step

1. **Channel filter.** A sample is used only if `s_axis_tvalid && enable && s_axis_tid == CHANNEL`. In single-channel mode the XADC sends only VAUX1 anyway. The filter costs almost nothing, though, and it protects you if the Wizard is ever set to sequence several channels.
2. **Accumulate.** `acc` (22 bits) adds each 12-bit code `s_axis_tdata[15:4]`. `cnt` (10 bits) counts 0…1023 and wraps naturally.
3. **On the 1024th sample** (`&cnt`, all ones):
   - `acc_next = acc + this sample` is the full 1024-sample sum.
   - Its top 16 bits, `acc_next[21:6]`, become the output. That equals sum ÷ 64, or average × 16.
   - `acc` resets to 0 for the next group.
   - `blk` counts outputs 0…511. When `blk == 511`, TLAST is raised and `blk` returns to 0.
4. **Output handshake.** The output register holds one sample. It is cleared when the FIFO takes it (`tvalid && tready`). It can be reloaded at the same clock edge, so throughput is never lost.

#### Why "keep bits 21..6" is division

Summing 1024 values and keeping bits 21–6 equals `sum >> 6`. Shifting right by *n* divides by 2ⁿ, and in hardware a shift is just a choice of which wires to keep. So averaging by a power of two costs zero logic. A divide by 1000 would need a real divider circuit. This is the "division by shifting" row of the concepts table, and the line to point at is:

```verilog
wire [15:0] avg_out = acc_wide[ACC_W+15 -: 16];  // = acc_next[21:6] for LOG2R=10
```

#### Why averaging adds bits (number representation)

Averaging N samples of uncorrelated noise reduces the noise standard deviation by √N. For N = 1024 that is √1024 = 32 = 2⁵, which is up to 5 extra effective bits. We keep 4 of them (12 → 16 bits). Real gain depends on how much the noise really is uncorrelated. The "noise floor, single sample vs 1024-sample average" measurement checks exactly this.

Output scaling: an input code `c` held constant produces `16·c`. Full scale (4095) gives 65,520 = `0xFFF0`. Mid-scale (2048) gives exactly `0x8000`.

#### Overflow: can the 22-bit accumulator overflow?

No. The maximum sum is 1024 × 4095 = 4,193,280, which is below 2²² = 4,194,304. The accumulator is sized `12 + LOG2R` bits for exactly this reason.

#### The drop policy (what happens if the FIFO is full)

AXI-Stream has one strict rule. Once `tvalid` is 1, `tdata` and `tlast` must not change until the receiver takes the beat. So if a new average is ready while the old one is still waiting:

- The **new** sample is discarded and `drop_count` increments.
- `blk` does **not** advance. Every packet the DMA sees is therefore still exactly 512 samples long, which matters because a short or long packet would make the DMA end early or report an error.

With a 1024-deep FIFO in front of the DMA, a drop can only happen if the DMA is not re-armed for more than a second. That is a software bug worth catching, and this counter catches it.

#### `enable` — clean starts

When `enable = 0`, everything is held at zero, including `drop_count`. The startup sequence in software is:

1. Arm the DMA (`XAxiDma_SimpleTransfer` into `buf[0]`).
2. Set `enable = 1`.

Without this, the decimator would start producing samples at power-up, before the DMA exists in software. The FIFO would fill, and `drop_count` would already be non-zero when your 10-minute test begins. **Read `drop_count` before you clear `enable`.**

#### `blk_toggle` — measuring the true sample rate in hardware

The pin flips each time a TLAST beat leaves the decimator. One full period (two edges) is 1024 samples, and one edge-to-edge interval is 512 samples:

```
fs = 512 / (time between consecutive edges)
```

The plan also has a software "buffer pin" toggled in the DMA ISR (`dbg_pins[1]`). Comparing the two on the scope shows the **interrupt latency**: the ISR edge lags the hardware edge by the FIFO drain time plus interrupt entry time, typically a few microseconds.

#### Changes from the skeleton in the project plan (worth mentioning in the viva)

| Plan skeleton | Here | Why |
|---|---|---|
| No `s_tready` port | `s_axis_tready = 1` | The XADC Wizard has a `tready` input; it must be driven |
| On a drop, overwrote `m_tdata` while `m_tvalid` was high | Keeps the old beat, drops the new one | Changing data while valid is an AXI-Stream protocol violation |
| `blk` advanced even when a sample was dropped | Advances only on accepted samples | Keeps every DMA packet exactly 512 samples |
| Comment about filtering on `tid` | Built in (`CHANNEL`, `USE_TID`) | |
| Free-running from reset | `enable` input | No drops before the DMA is armed |
| No `tkeep` | `m_axis_tkeep = 2'b11` | The AXI DMA's S2MM port has TKEEP; tying it explicitly avoids an IP-integrator warning |
| 16-bit drop counter | 32-bit | Fills one GPIO channel; can't wrap |
| — | `blk_toggle` | Hardware-accurate sample-rate measurement |

#### Parameters

| Parameter | Default | In simulation |
|---|---|---|
| `LOG2R` | 10 (R = 1024) | 2 (R = 4) keeps the sim tiny |
| `BLOCK` | 512 | 4–8 |
| `CHANNEL` | 17 (VAUX1 = 0x11) | anything |
| `USE_TID` | 1 | test both 0 and 1 |

The output is always the top 16 bits of the sum, padded with zeros on the right if the sum is narrower. With `LOG2R = 2` the sum is 14 bits, so the output equals `sum << 2`. Keep that in mind when writing expected values.

---

### 3.2 `pwm_dac.v` — turning a number into a voltage

**Job:** the Zynq has no DAC. A pin switching fast between 0 V and 3.3 V, with the right fraction of high time, averages to any voltage in between once an RC filter smooths it.

| Port | Dir | Width | Connects to |
|---|---|---|---|
| `clk` | in | 1 | FCLK_CLK0 |
| `resetn` | in | 1 | `peripheral_aresetn` |
| `duty` | in | 8 | `gpio_out` ch1 |
| `pwm_out` | out | 1 | Pmod JA1 → 1 kΩ → node → 1 µF → GND; scope CH2 on the node |

**How it works:**

- An 8-bit counter `cnt` counts 0…255 forever. One period is 256 clocks = 2.56 µs, a **390.6 kHz** carrier.
- `pwm_out = (cnt < duty_q)`, so in each period the pin is high for exactly `duty_q` clocks.
- **Average voltage = 3.3 V × duty / 256.** For example, duty 128 gives 1.65 V.

**Why the RC filter works:** the cutoff is f_c = 1 / (2π · 1 kΩ · 1 µF) ≈ 159 Hz.
- The 390 kHz carrier is ~2,450× above f_c, so it is cut to ~0.04 % (first-order roll-off).
- The 5–100 Hz signal is below f_c and passes.
- At 100 Hz there is some attenuation (≈ −1.5 dB), which you will see in the sweep.

**Why `duty_q`:** `duty` is copied into `duty_q` only on the last clock of a period. If software changed `duty` halfway through a period, that period would otherwise get a width that is neither the old value nor the new one, which shows up as a glitch. Latching at the period boundary makes every period clean.

**Why `pwm_out` is registered:** a comparator output drives the pin through a flip-flop, so there are no combinational glitches at the pin.

**Limits:** `duty = 0` gives 0 V. `duty = 255` gives 255/256 × 3.3 V ≈ 3.287 V. A true 100 % is not reachable, which is normal.

**How software uses it:** the decimated samples are 16-bit. Software writes the top 8 bits (`sample >> 8`) for the Day-1 passthrough, or a band-energy value later. The PWM shows what the ARM is outputting.

---

### 3.3 `axis_test_pattern.v` — a fake XADC with known answers

**Job:** it lets you debug the DMA path (Days 2–3) without the analog front end. Every sample's value is predictable, so if DDR contains anything else, the bug is in the data path, not the wiring.

| Port | Dir | Width | Connects to |
|---|---|---|---|
| `aclk`, `aresetn` | in | 1 | FCLK_CLK0, `peripheral_aresetn` |
| `mode` | in | 2 | `gpio_ctrl` ch2 bits [3:2] |
| `phase_inc` | in | 32 | `gpio_ctrl` ch1 |
| `m_axis_tdata/tid/tvalid/tready` | | | mux input `s1_axis` |

**How it works:** it is a DDS (direct digital synthesis) oscillator.

- A divider fires a `tick` every 104 clocks, which is 961.5 kSPS, the same rate as the real XADC. The decimated rate and block timing are therefore identical whichever source is selected.
- On each tick a 32-bit phase accumulator adds `phase_inc`. It wraps at 2³², which is one full waveform cycle:

  ```
  f_out     = phase_inc × 961,538 / 2³²
  phase_inc = round(f_out × 2³² / 961,538)      → 20 Hz: 89,335   50 Hz: 223,338   100 Hz: 446,677
  ```

- The top 12 bits of the phase are shaped into a code:

| `mode` | Shape | Use it to check |
|---|---|---|
| 0 | Ramp (sawtooth 0→4095) | **Data integrity.** Consecutive decimated samples increase by a nearly constant step and wrap. A missing, repeated or swapped sample is obvious. |
| 1 | Triangle | **Detection.** Strong fundamental, weak odd harmonics (3f at −19 dB). A 20 Hz triangle should say "rumble". |
| 2 | Square 0/4095 | **Overflow.** Full-scale input; Q15/Goertzel must not overflow. |
| 3 | Constant 2048 | **Exact arithmetic.** Every decimated sample must be exactly `0x8000`. |

- The output format matches the XADC: the code goes in `tdata[15:4]`, and `tid = 17`.

**Why triangle and not sine:** a sine needs a lookup table, and a triangle is just "invert the bits on the second half". For a frequency detector the fundamental dominates either way. A sine LUT can be added later if you want cleaner spectra.

---

### 3.4 `axis_src_mux.v` — choose the source

**Job:** `sel = 0` routes the XADC to the decimator. `sel = 1` routes the test pattern.

**Detail that matters:** the source *not* selected gets `tready = 1`, so it keeps streaming into nothing. If the XADC's stream were stalled instead, its internal FIFO would overflow and it would report problems or lose data when you switched back.

The mux is purely combinational. `aclk` and `aresetn` exist only so IP integrator associates the AXI-Stream interfaces with a clock (otherwise validation complains). **Change `sel` only while `enable = 0`.**

---

## 4. Putting it together in Vivado (block design checklist)

This is the Day 2 build. Day 1 needs only the PS, XADC (AXI-Lite only), one GPIO and `pwm_dac`.

### 4.1 Add the sources
1. *Add Sources → Add or create design sources* → add the four files in `rtl/`.
2. *Add Sources → Add or create constraints* → add `constraints/pynq_z2.xdc`.
3. In the block design: right-click the canvas → **Add Module…** → pick each module. Vivado recognises the `s_axis` / `m_axis` ports as AXI-Stream interfaces because of their names and the `X_INTERFACE_*` attributes.

### 4.2 IP settings

**ZYNQ7 Processing System** (run *Block Automation* first, with the PYNQ-Z2 preset)
- PL Fabric Clocks: FCLK_CLK0 = 100 MHz
- PS-PL Configuration → HP Slave AXI Interface: enable **S AXI HP0** (32-bit is fine)
- Interrupts: enable **Fabric Interrupts → PL-PS → IRQ_F2P**

**XADC Wizard**
- Interface: **AXI4Lite**, and tick **Enable AXI4Stream**
- Startup channel selection: **Single Channel**; timing mode **Continuous**
- DCLK = 100 MHz; check the summary shows ~961.54 KSPS
- Channel: **VAUXP1/VAUXN1**, **unipolar**
- Turn off alarms you don't need (over-temperature can stay)
- Make `Vaux1` external and name it **`Vaux1`**

**axis_src_mux** — `s0_axis` ← XADC `M_AXIS`; `s1_axis` ← `axis_test_pattern/m_axis`; `m_axis` → `decim_avg/s_axis`

**decim_avg** — defaults (LOG2R 10, BLOCK 512, CHANNEL 17, USE_TID 1)

**AXI4-Stream Data FIFO**
- Depth **1024**, clock mode **Common**, no packet mode needed
- At 939 S/s this holds about one second of data. That is far more than the few microseconds needed to re-arm the DMA, so there is a big safety margin.

**AXI Direct Memory Access**
- **Disable** Scatter Gather Engine (simple mode)
- **Disable** Read Channel (MM2S); **enable** Write Channel (S2MM)
- Width of Buffer Length Register: 14 bits or more (we need 1024 bytes → 11 bits)
- Memory map data width 32; stream data width 16 (set by the connection)
- Connect `M_AXI_S2MM` → `S_AXI_HP0` (via the SmartConnect/Interconnect that Connection Automation adds)
- Connect `s2mm_introut` → `IRQ_F2P` (use a *Concat* IP if you add more interrupts later)

**AXI GPIO × 3**

| Instance | Channel | Direction / width | Connects to |
|---|---|---|---|
| `gpio_out` | ch1 | output, 8 | `pwm_dac/duty` |
| `gpio_out` | ch2 | output, 4 | external port **`dbg_pins`** |
| `gpio_ctrl` | ch1 | output, 32 | `axis_test_pattern/phase_inc` |
| `gpio_ctrl` | ch2 | output, 4 | Slice bit 0 → `decim_avg/enable`; Slice bit 1 → `axis_src_mux/sel`; Slice bits [3:2] → `axis_test_pattern/mode` |
| `gpio_in` | ch1 | input, 32 | `decim_avg/drop_count` |
| `gpio_in` | ch2 | input, 2 | external port **`sw`** |

Use the **Slice** IP (`xlslice`) to split `gpio_ctrl` ch2 into fields.

**External ports** (names must match the XDC): `Vaux1`, `pwm_out`, `dbg_pins[3:0]`, `blk_toggle`, `sw[1:0]`.

### 4.3 Clocks and resets
Everything uses **FCLK_CLK0** and one **Processor System Reset** IP (`peripheral_aresetn`). A single clock domain means there are no clock-domain crossings to worry about.

### 4.4 Finish
Address Editor → *Assign All* → Validate Design (F6) → Create HDL Wrapper (let Vivado manage it) → Generate Bitstream → *File → Export → Export Hardware* (**include bitstream**) → `.xsa` for Vitis.

---

## 5. Register map for software (preview)

| What | Driver call (Vitis) | Values |
|---|---|---|
| PWM duty | `XGpio_DiscreteWrite(&gpio_out, 1, duty)` | 0–255 |
| Scope pins | `XGpio_DiscreteWrite(&gpio_out, 2, bits)` | bit0 busy, bit1 buffer, bit2 detect, bit3 LED0 |
| Control | `XGpio_DiscreteWrite(&gpio_ctrl, 2, v)` | bit0 enable, bit1 src_sel (1 = pattern), bits[3:2] mode |
| Test frequency | `XGpio_DiscreteWrite(&gpio_ctrl, 1, phase_inc)` | see §3.3 |
| Drop counter | `XGpio_DiscreteRead(&gpio_in, 1)` | must stay 0 |
| Switches | `XGpio_DiscreteRead(&gpio_in, 2)` | SW0 = float build, etc. |

**Watch for a software race.** The busy pin (main loop) and the buffer pin (ISR) share `gpio_out` ch2. A GPIO write replaces the whole channel, so two writers can undo each other. One example: main reads, the ISR toggles, then main writes back the stale value. Keep a shadow variable of the pin bits and disable interrupts around the main loop's update, or drop the ISR pin and use the hardware `blk_toggle` instead.

---

## 6. Pins (PYNQ-Z2) — checked against the official master XDC

| Signal | Board location | FPGA pin |
|---|---|---|
| Analog in (A0 → VAUX1) | Arduino A0 | E17 / D18 |
| `pwm_out` | Pmod JA pin 1 | Y18 |
| `dbg_pins[0]` busy | Pmod JA pin 2 | Y19 |
| `dbg_pins[1]` buffer | Pmod JA pin 3 | Y16 |
| `dbg_pins[2]` detect | Pmod JA pin 4 | Y17 |
| `blk_toggle` | Pmod JA pin 7 | U18 |
| `dbg_pins[3]` | LED0 | R14 |
| `sw[1:0]` | SW1, SW0 | M19, M20 |

All of these match TUL's master XDC (`PYNQ-Z2 v1.0.xdc`, from https://dpoauwgwqsy2x.cloudfront.net/Download/pynq-z2_v1.0.xdc.zip), checked 2026-11-03. Pmod pins 5/11 are GND and 6/12 are 3.3 V, so use GND for the scope ground.

---

## 7. Pitfalls to remember

- **Analog range:** A0 accepts 0–3.3 V through the onboard divider. Never go below 0 V. Use the 1 kΩ series resistor and check the generator on the scope first (High-Z load setting).
- **DMA packet length = buffer length.** The DMA is told 1024 bytes (512 × 2). The decimator's TLAST must arrive at exactly the 512th sample. If it arrives later, the DMA flags an error; if earlier, the transfer ends short. The testbench will check this.
- **Re-export after any hardware change** and update the Vitis platform (see `VITIS Details.md`).
- **The XADC Wizard's AXI-Stream also needs its AXI-Lite port** connected to the PS (the `XSysMon` driver uses it). Connection Automation does this.

---

## 8. What the tests will cover (next step)

| Module | Planned checks |
|---|---|
| `decim_avg` | Average is exact for constant/ramp inputs; TLAST exactly every BLOCK outputs; wrong-TID samples ignored; backpressure → data held stable, drop_count increments, packets stay BLOCK long; `enable` clears everything; full-scale input never overflows; `blk_toggle` edges |
| `pwm_dac` | High time = duty clocks for every duty 0–255; duty changes only at period boundaries; period = 256 clocks |
| `axis_test_pattern` | Tick spacing = CLKS_PER_SAMPLE; phase step; each mode's code; holds data under backpressure |
| `axis_src_mux` | Routing both ways; unselected source always ready |
| Integration | Pattern → mux → decimator chain, decimated values compared against a Python model |
