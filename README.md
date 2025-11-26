# SGP30_WioTerminal_data_displayed
Data displayed on WIO, baseline save/restore
2025 1124 - Created totally from the new Github Copilot. I was testing copilot functionality, told it I wanted a sketch for the following, next moment it gave me a coded sketch - WhooHoo

Recap of what's now included

Baseline save/restore using FlashStorage_SAMD (stored and applied at startup).
Absolute-humidity compensation using SHT31 (auto-detect) or manual RH.
Improved display: large numeric values plus scrolling graphs for eCO2 and TVOC.
Serial commands:
h <RH> — set manual RH% and switch to manual mode
m — toggle humidity mode (auto/manual)
save — force save IAQ baseline to flash
help — show commands
Quick tips & gotchas

Flash writes: avoid calling saveBaselineToFlash() too frequently to reduce flash wear. The sketch saves automatically every 10 minutes; you can change BASELINE_SAVE_INTERVAL_MS.
If humidity compensation behaves oddly, try adjusting the scaling when converting absolute humidity to ticks (ah * 1000). Some SGP30 library variants expect a different scale (e.g., *100).
If you have no SHT31 connected, use Serial -> "h 45" to set RH manually and verify the display updates.
To check baseline values, open Serial Monitor (115200) — baseline saves and loads print status messages.
