# RK3568 V1.7 Pin Plan

## Confirmed

| Resource | JP11 / connection | Voltage | Current use | Decision |
|---|---|---:|---|---|
| I2C4 | JP11 pins 1/3 | 1.8 V | MIPI CSI related | Backup only; level shifter required |
| I2C1 | JP11 pins 6/8 | 3.3 V | DSI GT911 touch | Do not use first |
| I2C3 | JP11 pins 2/4 | 3.3 V | LVDS touch DTS config | Verify runtime before use |
| PWM2 | - | - | Fan | Do not use |
| PWM7 | - | - | Infrared key decode | Do not use |

## Pending

- Confirm whether I2C3 is physically unused with the current MIPI display.
- Identify GPIO on JP11 that is not occupied by UART9, 5G, LED, or camera reset.
- Confirm a GND connection point for external modules.
