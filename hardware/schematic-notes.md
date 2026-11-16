# Schematic notes

These notes used to sit as free text on the schematic sheets. They were moved here so the sheets
stay clean for screenshots. Nothing here changes the circuit; it is the design reasoning behind it.
Only the sheet titles were kept on the schematic.

## Root sheet (sms.kicad_sch)

```
USB-C input + protection
3.3 V buck, modem VBAT + switch
```

```
ESP32-S3-WROOM-1 module
reset/boot, decoupling, LEDs
```

```
A7683E, VBAT caps, PWRKEY/RESET
SIM, antenna, modem USB-C
```

```
Block diagram:
  USB-C 5 V -> 3.3 V buck (ESP32, codec, shifter) and 3.8 V buck + load switch (modem VBAT, ESP32-switched)
  ESP32-S3 <-> TXS0108E <-> A7683E UART + control (1.8 V modem I/O)
  ESP32-S3 <-> I2S/I2C <-> ES8311 <-> analog audio <-> A7683E SPK/MIC
  A7683E -> nano-SIM, u.FL LTE antenna, separate USB-C for modem firmware updates

Signals between sheets go through hierarchical sheet pins, wired below. Power rails
(+5V, +3V3, +3V8, +1V8, GND) are power symbols and connect by name on every sheet.
```

```
TXS0108E 1.8 V <-> 3.3 V
UART + status lines
```

```
ES8311 codec + analog
audio to/from the modem
```

## Power (power.kicad_sch)

```
Every block is the manufacturer's reference circuit.

Input: USB-C J101 (5.1k Rd on CC1/CC2 = standard USB-C sink) -> F101 2 A resettable fuse -> +5V.
       D102 SMF5.0A clamps surges on +5V, U107 USBLC6 protects the ESP32 USB data lines.
       Needs a USB-C charger rated >= 1.5 A (worst case ~1.3 A at 4.75 V).
3V3:   U105 SY8089 per Silergy typical application (22uF in, 2.2uH, 2x22uF out, 22pF feed-forward).
       Vout = 0.6 V x (1 + 100k/22k) = 3.33 V (3.21-3.45 V worst case).
VBAT:  U106 SY8089, always on, divider 160k/30k from the SIMCom reference design -> 3.80 V (3.66-3.94 V).
       U108 TPS22965 load switch (4 A, for the A7670E 2G bursts) lets the ESP32 cut the modem supply (MODEM_PWR_EN, off by default via R105).
Sense: VBUS and VBAT dividers to ESP32 ADC1 (IO1 / IO2) so firmware can watch both rails
       (VBUS 5.5 V -> 1.76 V, VBAT 4.2 V -> 2.1 V).
PWR_FLAGs tell ERC where each rail is driven from (+3V8 is driven by U108's output pin).
```

```
3.3 V buck (ESP32, codec, level shifter): 0.5 A design load
```

```
Modem VBAT buck: 1 A design load (SIMCom asks for >= 1 A), always on
```

```
Modem power switch (TI TPS22965 typical application)
```

```
R105: ON must never float (TI); keeps the modem off while the ESP32 boots.
C115: 1 uF input cap at the switch (TI). C116 (CT): 10 nF soft-start ramp (limits inrush into the bulk caps).
VBIAS from VBAT_PRE. Its built-in output discharge (~300 Ohm) is too slow for SIMCom: see Q101 below.
```

```
VBAT discharge (SIMCom: VBAT falls in ~50 ms at switch-off)
```

```
Switch off -> Q102 off -> R117 turns Q101 on:
R116 22R empties the ~340 uF on VBAT
(90->10 % in ~15 ms). Switch on -> Q102
pulls Q101's gate low, no current.
```

```
Firmware power sequence (SIMCom A7670 series):
1. Read VBUS_SENSE; only start the modem above ~4.6 V.
2. MODEM_PWR_EN high; wait >= 100 ms (21 ms ramp + 40 ms Ton(wait)).
3. PWRKEY pulse >= 50 ms; STATUS goes high after ~7 s.
Shutdown: AT+CPOF (or PWRKEY 2.5 s), wait for STATUS low,
then MODEM_PWR_EN low. Before a restart, wait until
VBAT_SENSE < 0.1 V, then 2 s more.
```

```
Test pads: TP101-TP104 on each rail, TP105 GND,
TP106 scope ground loop (DNP, through-hole).
```

## ESP32-S3 (mcu.kicad_sch)

```
3V3 decoupling: C201 22uF + C202 100nF right next to U201 pin 2.

EN / RESET: R201 + C203 (10k / 1uF, Espressif reference) delay EN at power-up
so 3V3 is stable before the chip boots. SW201 pulls EN low = reset.

BOOT: hold SW202 (IO0 low) and tap RESET to enter the USB bootloader.
IO0 has an internal pull-up, so no resistor is fitted.
```

```
Logic-analyzer header J201 (DNP, 1x8 2.54 mm):
taps the 3.3 V side of the level shifter, to check
the modem power-up timing and the UART with a
cheap 8-channel analyzer. Its labels join the
hierarchical labels of the same name.
```

```
Status LEDs (driven by firmware)
```

```
Pin plan (rev 0.6, chosen so every group runs straight on the PCB):
- IO19 / IO20: native USB D- / D+ (USB-C on the power sheet)
- Bottom row, left: codec. IO3 I2S DOUT, IO46 LRCK, IO9 DIN, IO10 BCLK, IO11 MCLK, IO12 I2C SCL, IO13 SDA
- Bottom row, right: IO14 modem RESET (NPN), IO21 NETLIGHT, IO47 RXD (ESP TX), IO48 TXD (ESP RX)
- Right side: IO38 RTS, IO39 CTS, IO40 RI, IO41 DTR, IO42 STATUS; RXD0 free,
  TXD0 = PWRKEY (through an NPN; the modem is unpowered while the ESP32 boots)
- IO2 / IO1 (ADC1): VBAT and VBUS voltage monitoring. IO7: modem power switch enable. IO4 / IO5: status LEDs
- IO35-37 are used by the octal PSRAM on N16R8: never use
- IO46 (strapping) only carries LRCK, an ESP32 output, so it is low at boot; IO45 left free
```

## Modem (modem.kicad_sch)

```
UART: 100R in series on TXD/RXD at the module (SIMCom).
The other UART lines go straight to the level shifter sheet.

Audio lines: 33 pF to GND right at the module pins (RF filtering).

SIM: 22R series + one TPD1E10B06 ESD diode per line near the holder, 100n + 33p
on SIM_VDD (SIMCom reference). Keep the holder far from the antenna.
The ESD diodes join the SIM lines through the SIM_* labels.
```

```
Modem USB-C (J303): only for modem firmware updates and SIMCom
debug tools. It does not power the board. VBUS just tells the
modem a host is present.
```

```
Antenna: short 50R trace (< 20 mm) from ANT_MAIN to the u.FL.
Pi network R312 / C309 / C310 is for matching (0R + DNP until
tuned), C311 100pF per SIMCom for ESD, D302 low-capacitance
TVS optional (DNP).
```

```
VBAT = SIMCom Figure 4: TVS D301 and bead FB301 at the entry,
then >= 200 uF bulk (C301 220uF tantalum + C302 100uF) and
1u/100n/33p/10p at the pins. Route VBAT >= 2 mm wide.
R315 (0R 1206) is a current link: remove it to put an ammeter
or a bench supply across its pads. TP305 sits at the module
VBAT pins, TP306 is a scope ground loop (DNP).
```

```
PWRKEY / RESET: active-low inputs pulled up to VBAT inside the
module. The ESP32 drives them through NPN transistors (SIMCom
reference: 4.7k / 47k / 1k). PWRKEY low 50 ms = power on,
2.5 s = power off. RESET low 2.5 s = reset. Never pull both low
at the same time. No capacitor > 10 pF on these pins.
```

```
Test pads (SIMCom asks for USB_BOOT, UART_LOG_TX, VDD_1V8 and GND).
TP301 (USB_BOOT) forces the modem's USB download mode at power-up
(level per the A7670 hardware design guide).
```

## Level shifter (levelshift.kicad_sch)

```
The A7670E I/O runs at 1.8 V (its VDD_1V8 output), the ESP32-S3 at 3.3 V.
TXS0108E (U401) translates 8 lines in both directions automatically: A side = 1.8 V
(VCCA, must be the lower supply), B side = 3.3 V. OE is tied to VDD_1V8, so the
shifter only turns on once the modem is running. While the modem is off the B side
floats: enable the ESP32 internal pull-ups/downs on these pins in firmware.
TXS0108E is fine for UART speeds; it does not like strong pull-ups on its lines.
C401 / C402: 100 nF on VCCA / VCCB, next to the pins.
```

## Audio (audio.kicad_sch)

```
Bring-up options (DNP = footprint only, fit it if needed):
R509 0R bridges AVDD_IN -> AVDD. If 3V3 noise reaches the
audio, remove R509 and fit U502 + C513 (3.0 V LDO).
R510 0R is the only link between GNDA and GND: lift it to
test star grounding.
C514/C515/C516: RF filter on the downlink divider,
100 pF to 1 nF, if LTE buzz shows up.
Test pads TP501-TP508: probe each stage during a call.
TP509: ground loop for the scope clip.
```

```
Modem earpiece out -> codec mic in
```

```
Codec line out -> modem mic in
```

```
The modem has no digital audio, only an analog earpiece
output (SPK) and a mic input (MIC). The ES8311 converts
between those analog signals and I2S for the ESP32.

Modem -> ESP32: SPK1P/N -> 10k + 2.2k divider (about -20 dB)
-> 1 uF -> codec MIC1P/N. Codec PGA gain and AT+CLVL set the
final level.
ESP32 -> modem: codec OUTP/N -> 10k + 220R divider (about
-40 dB, line level down to mic level) -> 1 uF -> modem
MICP/N. Tune with AT+CMICGAIN and the codec volume.
Divider values are starting points: check levels on the bench.

Codec I2C address 0x18 (CE tied to GND).
```

```
Analog supply (AVDD) and ground options
```
