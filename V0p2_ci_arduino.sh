#!/bin/sh -e
# Build with the default config with Arduino IDE
arduino --verify --board opentrv:avr:opentrv_v0p2 $PWD/Arduino/V0p2_Main/V0p2_Main.ino
