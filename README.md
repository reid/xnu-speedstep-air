# This project is dead. Use at your own risk. The authors can no longer support this software.

I no longer own a MacBook Air Rev. A, so I can no longer help folks with this software.

If you'd like to take over this project, please contact me.

# xnu-speedstep-air

xnu-speedstep-air is a kernel extension to keep my MacBook Air from overheating.

It's totally free and works with Mac OS X Lion.
Compare to [CoolBook][] which doesn't work with Lion and costs $10.

It's basically [xnu-speedstep][] with bugfixes and preloaded voltage tables for the MacBook Air Rev. A.

## Why?

I used to own a MacBook Air Rev. A, which is well-known for its [thermal design problems][psm].

Fortunately, if you reduce the voltage to the processor, the system can
run much cooler without thermal protection events happening
(like kernel_task emitting no-ops or core shutdown).

I used to use [CoolBook][] for undervolting the system, but it costs $10. When I upgraded to Lion, CoolBook stopped working. Instead of downgrading to Snow Leopard, I decided to save my money by hacking this together.

HUGE THANKS to [Prashant Vaibhav][msq], [wbyung][], and Superhai for writing this code. I simply made a few changes to get it working on Lion and added voltage overrides for my MacBook Air Rev. A.

You've saved my sanity and tought me a tiny bit about hacking OS X.

## Usage

**You should understand what this does: if you follow these instructions, you're undervolting your CPU to 892 - 940 mV.** I use it on my MacBook Air from 2008 without a problem.

If you're using a different computer, you need to change or remove the PStateTable entry in Info.plist.
I only tested it with the MacBook Air Rev. A., computers made after 2008 are very unlikely to work.
Failure to be careful may cause hardware instability, crashes, or possibly damage.

Read the LICENSE file. This code is provided AS-IS and at your own risk.

Build the Source using Xcode. You may need to change your build product location (see below).

Run deploy.sh, type your password.

Type: `sudo kextload /System/Library/Extensions/IntelEnhancedSpeedStep.kext`

Verify it worked: `sudo dmesg | grep IntelEnhancedSpeedStep`

## Xcode 4 and build products

In Preferences, select the Locations tab.

Click Advanced.

From the dropdown, select: Place build products in locations specified by targets

You'll now get the kext in build/.

[Coolbook]: http://coolbook.se/
[xnu-speedstep]: http://code.google.com/p/xnu-speedstep/
[psm]: http://paulstamatiou.com/putting-an-end-to-macbook-air-core-shutdown
[msq]: http://www.mercurysquad.com/
[wbyung]: http://forums.macrumors.com/showthread.php?t=751657
