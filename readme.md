# Simple Block Device Driver
Implementation of Linux Kernel 5.4.X simple block device.

## Build
- regular:
`$ make`
- with requests debug info:
uncomment `CFLAGS_sbdd.o := -DDEBUG` in `Kbuild`

## Clean
`$ make clean`

## References
- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Linux Kernel Development](https://rlove.org)
- [Linux Kernel Teaching](https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html)
- [Linux Kernel Sources](https://github.com/torvalds/linux)

# Notes regarding the test case
Here comes a solution for two tasks out of three. The third one doesn't look much harder
as it just requires you to map blocks based on number of 'raid' devices but the deadline is nigh. 

It took me approximately a day and a half of really lazy reading of kernel documentation and learning
the aspects of the blkdev subsystem and two rather busy evenings to implement my solutions in a form of code.

Testing can be done that way:
- `insmod sbdd.ko mode=0` simply goes for running the preexisting code
- `insmod sbdd.ko mode=1 proxy=<path>` shows my implementation for the first task, where path is a path to a blkdev to be proxied
- `insmod sbdd.ko mode=2 raid=<pathes..>` shows my implementation for the second task, where pathes is a comma-separated list of
blkdevices that a used to mirror all the requests. No original data syncing or else.

You can also dynamically switch modes. You just setup the required param (like 'proxy' or 'raid') and the trigger reset by setting
the 'mode' param