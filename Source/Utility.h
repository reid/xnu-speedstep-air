#ifndef _UTILITY_H
#define _UTILITY_H

#define LOGPREFIX "IntelEnhancedSpeedStep: "

#define dbg(args...)	do { if (DebugOn) { IOLog (LOGPREFIX "DBG   " args); } } while(0)
#define warn(args...)	do { IOLog (LOGPREFIX "WARN  " args);  } while(0)
#define info(args...)	do { IOLog (LOGPREFIX "INFO  " args);  } while(0)


#define releaseObj(x)	{ if (x) { (x)->release(); x = 0; } }

#define INTEL_MSR_PERF_CTL	0x199
#define INTEL_MSR_PERF_STS	0x198

#define CTL(fid, vid)	(((fid) << 8) | (vid))
#define FID(ctl)		(((ctl) & 0xff00) >> 8)
#define VID(ctl)		((ctl) & 0x00ff)

#define abs(x)		(((x)<0)?(-(x)):(x))

#endif // _UTILITY_H