#!/bin/sh
sudo kextunload /System/Library/Extensions/IntelEnhancedSpeedStep.kext
sudo rm -rf /System/Library/Extensions/IntelEnhancedSpeedStep.kext
sudo cp -R Source/build/Debug/IntelEnhancedSpeedStep.kext /System/Library/Extensions/
sudo chown -R root /System/Library/Extensions/IntelEnhancedSpeedStep.kext
sudo chmod -R 755 /System/Library/Extensions/IntelEnhancedSpeedStep.kext
echo "Ready: kextload /System/Library/Extensions/IntelEnhancedSpeedStep.kext"

