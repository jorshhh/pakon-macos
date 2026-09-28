# TLB.dll findings — the F-135 / F-135+ engine

_Source: Ghidra 12.1.4 decompile of `TLB.dll` 3.1.0.28 (sha256 `5866ec56…`), the
135-line engine (earlier RE in this repo used `TLA.dll`, which is the F-235's).
Function names were recovered from the engine's own error-ID table (`FN_b…`).
Facts only; no Kodak code is reproduced. Frames marked X match
`resources/scan*.pakscan`. Date: 2026-09-28._

**Addresses.** SCN = `0x24` (base) / `0x44` (Plus): FPGA register banks + motor.
LOW = `0x20` / `0x40`: LEDs, temperatures, DX, FIFOs. Model detection probes
`0x44`, `0x46`, `0x24`, `0x26` in that order; a controller that answers only at
its bootloader address still counts as present.

## Corrections to our own docs

| Was | Is |
|---|---|
| bank 0x82 reg 6 = "Height" | integration / line period (≤ 0xFFD); reg 4 = pixel start (EEPROM Offset), reg 5 = start + height |
| bank 0x82 reg 9 = brightness candidate | status LEDs |
| LOW 0x8B/0x8C/0x8D/0x8F = exposure / LED geometry | temperature threshold bands (lamp/motherboard warn/fault), 1/16 °C |
| 0x8A = AcquireLine | ResetFifos (with host `02 04 10 01 84 02`) |
| SCN 0xA5 = motor calibration | motor speed (u16); 0xA0 forward, 0xA1 reverse, 0xA2 stop |
| illumination gated by the running motor | calibration is static, motor stopped; LEDs are turned on by LOW 0x80/0x81/0x82 |
| EEPROM region 2 = LED current/duty | motor-adjust words (3 bases × {Adjust, AdjustDrag, Adjust_Ir, AdjustDrag_Ir}) |
| NegMatrix unused / F-235 LUT+3×4 path | F-135 path = density LUT then per-unit 3×10 NegMatrix in density space |


## TLB.dll 3.1.0.28 — scan flow (X = matches pakon-macos resources/scan*.pakscan)
Addresses: FUN_1000afd0 probes 0x44, then 0x46 -> Plus; else 0x24/0x26 -> base.
SCN = 0x24/0x44 (FPGA banks + motor). LOW = 0x20/0x40 (LEDs, temps, DX, FIFOs).

### Frames
CMD `04 03 a 00 c`; WRITE `02 n+3 a n reg data`; READ `01 03 a n reg`; FPGA reg `02 06 SCN 03 bank reg v16`.
After each CMD/WRITE (except cmds 0x01/0x0D/0x25): poll `03 01 a` until bit0 clear, <= 44 tries.
Reply status 3/6/9 and flags 0x01/0x10 -> retry; 1/2/4/5/7 hard error; 0/8 OK.

### Prepare (FN_bBeforeScan 1002dbd0)
1. wake interrupt thread (clear pending events)
2. ResetFifos: `02 04 10 01 84 02`, `04 03 LOW 00 8a` X  (0x8a = ResetFifos, not AcquireLine)
3. recalibrate only if no record / older than DarkPointCorrectIntervalMinutes / forced
4. FN_bDrvPutCcdFpgaSettings: bank82 reg0 bit 0x100 = IR mode X (0x160/0x161); reg6 = integration period X (0x0c1a); reg4 = EEPROM Offset; reg5 = Offset+height
5. AtoD offsets bank84 regs5-7 sign-mag (bit 0x100 neg, ±255); gains bank84 regs2-4 (<= 63)
6. LampOn
7. motor start, no wait: `02 05 SCN 02 a5 speed16`, `04 03 SCN 00 a0` X (0xa1 reverse; 0xa5 = speed)

### Run (FN_iScanStrips 10029b80)
- one overlapped ReadFile over the whole ring; driver keeps reading 0x86
- start acquisition: FPGA reg0 = ctrl|1, then `02 06 LOW 03 91 v16 mode` X (`91 10 00 01`)
- NO per-line commands. Later strips resend only 0x91. Stop acquisition = clear reg0 bit0.

### Interrupt-service thread (Thread_PpbInterrupt 1002eb70) — the missing "housekeeping"
- polls `03 01 10` every 1 ms at high priority while scanning (200 ms idle)
- reply bit 0x80 -> for LOW and SCN: `01 03 a 01 02`; if flags & 0x80, ack `02 05 a 02 06 00 <status>` X (02052002060020)
- LOW status & 0xa4 -> `01 03 LOW 1e 90` (30-byte sensor read) X
- LOW status & 0x5b -> temps regs 0x83/0x84/0x88 (x 0.0625 °C)
- status LEDs = bank82 reg9 X

### Light (FN_bDrvLampOn 1002c5f0)
- LOW 0x80 enable: bit0 visible, bit1 IR X
- LOW 0x81 5 bytes currents [B, IR, R, 00, G]
- LOW 0x82 12 bytes u16 [dB, dIR, dR, 0, dG, period], clk 833333.3
- ceilings R/G/B/IR: base 6/8/8/0 (IR on 8/8/8/8); Plus 4/20/20/0 (IR on 8/24/24/8)
- on from BeforeScan to AfterScan; nothing refreshes it; motor does not gate it.
  Likely (inferred) cause of dark calibration: 0x81/0x82 never sent non-zero.
- LOW 0x8b/0x8c/0x8d/0x8f = temperature thresholds (LampTemp*, 1/16 °C), NOT exposure.
- `d0 00` / `d1 01` (TEC) also sent on the base F-135 X

### 0x91 value and motor speed (FN_bSetF135 10010760)
- v91 = round(A*clk*0.01/(E*B)); IR: 0.005 and E_ir
- E = 0x913/0xd9d/0xff0; E_ir = 0x60d/0x913/0xc1a
- default MotorSpeed = 100*v91 (matches F-135+ EEPROM)
- run speed = MotorSpeed(_Ir) * MotorAdjust(/_Ir/Drag/Drag_Ir)/1000, adjust clamp 900-1100 (EEPROM section B: 3 bases x 4)
- speed clamps: base 400-9500, Plus 1000-32766
- UNRESOLVED: A, B (DPI+0x60/+0x68); why base F-135 capture sends 0x0010

### End of film: decided on the host from pixels; no scanner signal
- each line's first word LSB must be 1, else EC_DRV_LostSync (or FifoOverflow if host status 0x02)
- line level (green? inferred) vs DetectWhite_G / DetectFilm_G, rebased from first 3 lines
  film starts: level < DetectFilm; film ends: level >= DetectWhite
- NoFilmTimeOut = seconds; ScanPacketReadyTimeOut = ms

### Teardown (FN_bAfterScan 1002a900) X
1. `02 04 LOW 01 80 00`  2. ResetFifos  3. `04 03 LOW 00 92`, wait 20 ms  4. `04 03 SCN 00 a2`
No motor-speed write before 0xa2, but FN_iScanStrips has already cleared FPGA bank 0x82 reg 0 bit 0 (acquire). The F-135+ captures show the same order (0x61 → 0x60, then a2). The August "write the idle speed register 0" fix was this FPGA write, so the finding and the OEM agree.

## TLB.dll 3.1.0.28 — calibration (read from code unless [INF])
- STATIC: open gate, motor stopped. No film-drive cmd anywhere under CalibrateBefore/FindCorrections (1001daa0/10021590); only optional focus steppers (flag 0x40, addr 0x28). Light switched on explicitly by FN_bDrvLampOn. "light only while motor runs" is wrong for TLB. pakon-macos likely never wrote 0x81/0x82 [INF].
- Preconditions: wait <= 300 s for lamp temp stable (1002cf10) else EC_LampWarmUpFailure; refuses positive film (colour 2) and non-35mm.
- Full LED search only if flag bit0 or > 72000 s (20 h) since FullLightCorrections (FUN_10010250); else reuse stored currents/duties.
### Phases (CalibrateLEDs 10020dc0)
1. window: first 6 px optically black (3 dual-tap); active cols start at dpi.offset - 6
2. lamp off (LOW 0x80 = 0)
3. dark offset: gain code 13 (g=1.2), offsets start 10/10/10, <= 8 iters x 32 lines, black-pixel mean target 300 ±32, off += round((mean-300) * -0.0133929)
4. fixed-pattern dark: 128 lines averaged -> per-column dark; 128 summed -> black & active means
5. LED current search (full only): current from 1, +1 per 32-line pass until column peak > cap (R,G 64000; B 65500; IR 40000) or ceiling; then duty = (n-1)/n
6. duty refine: <= 32 iters, duty *= 63968/peak (IR 39968), converged when peak in [63936,64000] (IR [39936,40000]); 2 converged passes 200 ms apart; if short: current+1, at ceiling A/D gain code+1 (max 0x3e); else EC_InsufficientLight
7. fixed-pattern bright (formula below)
8. IR mode only: IR LED alone; visible dark tables -> v[c] - (min - 300)
9. save to registry
### Per-column (1001f550; inputs are 128-line sums)
den = (B[c]-D[c]) - (Bmask-Dmask); gain[c] = min(125*2^32/den, 0x3ffff) (IR 0x7530000000)
"prefix terms" = drift of optically black pixels between bright and dark passes.
Per line: 64000*65536/(b-d-Δblack) -> 16.16 fixed point, open gate -> 64000 (IR 60000).
Smear = 65536*(Bmask-Dmask)/(Bactive-Dactive), kept only if 1..699.
### Film-base densities (FUN_10020230): calib duty capped at 1/10^D, scan duty = open-gate duty * 10^D
colour neg R0.144 G0.40 B0.715 IR0; B&W C-41 0.10/0.25/0.25/0.08; other (positive at scan, B&W) 0/0.03/0/0.08
### Encodings
- LampOn: LOW 0x80 bit0 vis bit1 IR; 0x81 [B,IR,R,0,G]; 0x82 u16 [dB,dIR,dR,0,dG,period]; period = IntegrationTime*0.6 (clk 833333.3 Hz); duty ticks <= period-2; settle waits WaitForLamp_*
- ceilings R/G/B/IR: base 6/8/8/0 (IR on 8/8/8/8); Plus 4/20/20/0 (IR on 8/24/24/8)
- A/D gain bank84 regs2-4: code = round((1-1/g)*75.6), g in [1,6]; 13 = the "13/13/13"
- A/D offset bank84 regs5-7 sign-mag (bit8 neg, ±255)
- bank82: reg6 integration time (<= 0xffd); reg4 pixel start; reg5 start+height (max 0x848 single, 0x424 dual tap); reg0 bits 0x001 acquire, 0x002 dual-tap, 0x100 IR, 0x060 init (-> 0x160/0x161); reg9 status LEDs
- temps (1002d190): LOW 0x8F lamp warn, 0x8C lamp fault, 0x8B motherboard warn, 0x8D motherboard fault; 0x8E lamp setpoint (LampTempWorking 37..48 °C); then LOW 0xD0=0, 0xD1=1 (TEC). 0.0625 °C/count. Readback LOW 0x84 setpoint, 0x88 lamp+mobo temps.
### Registry …\TLB\Scan\DpiBase{4,8,16}_35
Gain_R/G/B, Offset_R/G/B, Current_R/G/B/Ir, DutyCycle_* (film), DutyCycleOpenGate_*, FullLightCorrections, SpliceDarkness, DetectWhite_G, DetectFilm_G. Per-column tables in memory only.
### Open
16-bit multiplier in scan MMX kernel (FUN_100246d0) from 16.16 table; SmearC ramp (IR); dpi+0x90 flag (LOW 0x89); defaults of WaitForLamp_* and temp thresholds.

## TLB.dll 3.1.0.28 (sha256 5866ec56…) — EEPROM use and 135-line colour path
[C] = read from code/disassembly, [I] = inferred

### EEPROM -> engine (FN_bReadEEPromToRegistry 0x10016a90, FUN_10016860, FUN_10016610) [C]
- Per base (4/8/16): Offset, MotorSpeed, MotorSpeed_Ir from section A 0x014/0x01a/0x020.
- Section B per base, 4 words each (0x808/0x810/0x818): MotorAdjust, MotorAdjustDrag, MotorAdjust_Ir, MotorAdjustDrag_Ir; clamped 900..1100.
  -> the byte where the base F-135 differs (0x80C) is base-4 MotorAdjust_Ir (1008 vs 1000).
- NegMatrix (A 0x026) / PosMatrix (A 0x09e) -> colour object, unless a client matrix file is loaded.

### Offset = CCD pixel-window start (FN_bDrvPutCcdFpgaSettings 0x1002c340, scanner PIC) [C]
- FPGA bank 0x82: reg4 = Offset, reg5 = Offset + height, reg6 = line period (<= 0xFFD).
- reg6 is NOT height (pakon-macos REGISTERS.md is wrong): 0x0C1A = base-16 IR line period.
- Checks: Offset >= 6 (>= 3 half mode), Offset + height <= 0x848. Default: height 2000, offset 62, period 0xFFD.

### Motor speed (FN_bBeforeScan 0x1002dbd0 -> FN_bDriveMotorAdvanceFilm 0x1000b6d0) [C]
- speed = MotorSpeed[_Ir] * MotorAdjust[Drag/_Ir] / 1000 -> scanner-PIC reg 0xA5; then cmd 0xA0 forward / 0xA1 reverse; timed moves end with 0xA2.
- Reference names "SetMotorCalibration"(0xA5) and "StopFilmDrive"(0xA1) are wrong.
- Clamps: Plus 1000..32766, base 400..9500.
- 0x91 trigger value = round(A*833333.3*0.01/(P*B)) (0.005 with IR), P = 0x913/0xD9D/0xFF0 per base (FN_bSetF135 0x10010760); A, B unresolved.

### Colour negative path [C]
- PIColorCorrectColNegPlanarScan/…Save: GetProcAddress'd, never called.
- Real path: FN_bLoadImageFromBuffer 0x10026c90 (flag 0x10):
  1. per-plane LUT on planar 16-bit: LUT[i] = 3500*log10(16383/i), 16384 entries, LUT[0]=0x3FFF (FUN_1000dfc0).
  2. 3x10 on density (FN_bReadMatrix_3x10 0x1000d880): NegMatrix for colour neg and both B&W; PosMatrix for positive (then PIColorCorrectColRevPlanar).
     out = m0R+m1G+m2B+m3R²+m4G²+m5B²+m6RG+m7BR+m8GB+m9, +0.5, clamp 0..4095 -> 12-bit.
     Term order is RG, BR, GB (reference says RG, GB, BR).
  3. scale/rotate -> 12-bit RPD/sRGB profiles + Ansel scene balance.
- NegMatrix constants positive -> no film-base subtraction here; base likely neutralised by light calibration [I].
- Roll balance FN_bKcdfsCorrections 0x10034a60: 3 roll values (film base?) [I] through same LUT+matrix; Ansel roll analysis over <=40 pictures.
- ClientColNegLut.txt + ClientColNegMat_3x10.txt override only if both exist; shipped _ClientColNegLut.txt (leading underscore) is never loaded.
