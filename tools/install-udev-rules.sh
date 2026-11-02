#!/bin/sh
# Installs the udev rule that gives the logged-in user access to USB Bluetooth
# controllers and applies it to the ones already plugged in.
set -eu

rules=70-oticon-asha-adapters.rules

sudo install -m 644 "$(dirname "$0")/$rules" "/etc/udev/rules.d/$rules"
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --settle
echo "installed /etc/udev/rules.d/$rules"
