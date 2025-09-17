# Overview
- Displays temperature (°C), humidity (%RH), battery level (%)
- Shows pressure and computed altitude (m) using QNH from METAR (EDDB)
- Clock/date via NTP and timezone set to NASDAQ time (EST5EDT)
- Simple stock quote viewer (NVDA / ASTS) from a custom HTTP JSON API
- Button A toggles the stock symbol, Button B forces a refresh
- Beeps on button actions
    
# Requirements

- Arduino IDE (https://www.arduino.cc/en/software/)
- M5stack Stick C Plus 1.1
- M5 HAT ENV III sensors
- NTP with NASDAQ timezone
- AWS Lambda (for proxying Stock Exchange; so, the HW will be fetching stock prices from an AWS Lambda ;) )

# Project

<img width="1280" height="960" alt="image" src="https://github.com/user-attachments/assets/12e84a29-deae-4689-af71-b3568de80df1" />

# Hardware Resources

<img width="800" height="800" alt="image" src="https://github.com/user-attachments/assets/7cc8add7-7bf6-490a-9e3c-4833bcbe3408" />

- ref: M5StickC PLUS ESP32-PICO Mini IoT Development Kit (https://docs.m5stack.com/en/core/m5stickc_plus)

<img width="800" height="800" alt="image" src="https://github.com/user-attachments/assets/c699f97c-ee1c-4496-bd90-0b499fb73d7a" />

- ref: M5StickC ENV III HAT (SHT30, QMP6988) (https://docs.m5stack.com/en/hat/hat_envIII)
