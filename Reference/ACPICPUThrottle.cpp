/* ACPICPUThrottle.c: ACPI Processor Performance Control Module for MacOS X
 *
 * Copyright (C) 2006 Niall Douglas <http://www.nedprod.com/>
 * Voltage multiplier support (C) 2008 Prashant Vaibhav <mercurysquad@yahoo.com>
 * AMD direct drive taken from FreeBSD acpi_ppc driver  Copyright (C) 2004,2005 FUKUDA Nobuhiko <nfukuda@spa.is.uec.ac.jp>
 * Intel direct drive taken from EnhancedSpeedStep driver Copyright (C) 2006 keithpk
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Reference:
 *   - ACPI Specification Revision 2.0c, pp.232-236
 *
 * Sysctl:
 *   - kern.cputhrottle_curfreq: Current CPU frequency (in Mhz)
 *   - kern.cputhrottle_verbose: Print lots of debug info to system.log
 *   - kern.cputhrottle_freqs: List of space separated frequencies available (in Mhz)
 *                             Some version text and contact info
 *                             Then a series of Px prefixed available powerstates in the form:
 *                                Px, <clock>Mhz, <power>mW, <cpu latency>us, <bus latency>us (ctrl=0x<reg to write>, status=0x<reg to read>)
 *
 * This module compiles for powerpc but is pretty useless there! :)
 */

/**********************************************************************/
#include <mach/mach_types.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <IOKit/IORegistryEntry.h>

#if defined(__i386__) || defined(__amd64__)
#include <i386/cpuid.h>
#include <i386/proc_reg.h>  //for rdmsr64 and friends
#include <i386/apic.h>


/*
 * AMD K8 Cool'n'Quiet
 *
 * Reference:
 *   - BIOS and Kernel Developer's Guide
 *     for the AMD Athlon 64 and AMD Opteron Processors, 26094 Rev.3.12
 *   - linux-2.6.3/arch/i386/kernel/cpu/cpufreq/powernow-k8.[ch]
 *   - acpi_ppc for FreeBSD 5.x and 6.x kernels by FUKUDA Nobuhiko
 */

#define MSR_FIDVID_CTL		0xc0010041
#define MSR_FIDVID_STATUS	0xc0010042

#define INTEL_MSR_VID_MASK 0x00ff
#define INTEL_MSR_FID_MASK 0xff00

#define write_control(qw)	wrmsr64(MSR_FIDVID_CTL, (qw))
#define read_status()		rdmsr64(MSR_FIDVID_STATUS)

#define control_irt(dw)		(((dw) >> 30) & 0x3)
#define control_rvo(dw)		(((dw) >> 28) & 0x3)
#define control_pll(dw)		(((dw) >> 20) & 0x7f)
#define control_mvs(dw)		(((dw) >> 18) & 0x3)
#define control_vst(dw)		(((dw) >> 11) & 0x7f)
#define control_vid(dw)		(((dw) >> 6) & 0x1f)
#define control_fid(dw)		((dw) & 0x3f)

#define status_vid(qw)		(((qw) >> 32) & 0x1f)
#define status_fid(qw)		((qw) & 0x3f)

#define count_off_irt(irt)	IODelay(10 * (1 << (irt)))
#define count_off_vst(vst)	IODelay(20 * (vst))

#define FID_TO_VCO_FID(fid)	(((fid) < 8) ? 8 + ((fid) << 1) : (fid))

#define write_fidvid(fid, vid, cnt)	\
	write_control(((cnt) << 32) | (1ULL << 16) | ((vid) << 8) | (fid))
#define READ_PENDING_WAIT(qw)	\
	do { (qw) = read_status(); } while ((qw) & (1ULL << 31))



// Names and numbers from IA-32 System Programming Guide
#define MSR_PERF_STATUS		0x198
#define MSR_PERF_CTL		0x199

#endif

template<typename type> class Ptr
{
protected:
	type *v;
	Ptr(type *_v=0) : v(_v) { }
public:
	~Ptr()
	{
		if(v) v->release();
		v=0;
	}
	Ptr &operator=(type *_v)
	{
		if(v) v->release();
		v=_v;
		return *this;
	}
	type *operator->() { return v; }
	type *operator *() { return v; }
	const type *operator->() const { return v; }
	const type *operator *() const { return v; }
	bool operator!() const { return !v; }
private:
	class Tester;
public:
	operator Tester *() const
	{
		if(v) return (Tester *) 1;
		return 0;
	}
};

class OSObjPtr : public Ptr<OSObject>
{
public:
	OSObjPtr(OSObject *r=0) : Ptr<OSObject>(r) { }
};
class IORegPtr : public Ptr<IORegistryEntry>
{
public:
	IORegPtr(IORegistryEntry *r=0) : Ptr<IORegistryEntry>(r) { }
};
class IORegIt : public Ptr<IORegistryIterator>
{
public:
	IORegIt(IORegPtr &r, const IORegistryPlane *plane)
	{
		v=IORegistryIterator::iterateOver(*r, plane);
	}
	IORegIt(IORegistryEntry *r, const IORegistryPlane *plane)
	{
		v=IORegistryIterator::iterateOver(r, plane);
	}
};

/**********************************************************************/

#pragma pack(1)
struct ACPIResource
{
	unsigned char type;
	unsigned short dataitemlen;
	union
	{
		struct GenericReg
		{
			unsigned char _ASI;		// Address Space ID
			unsigned char _RBW;		// Register Bit Width
			unsigned char _RBO;		// Register Bit Offset
			unsigned char reserved0;
			IOACPIAddress _ADR;		// Register Address
		} genericReg;
	} data;
};
#pragma pack()

struct acpi_px_reg
{
	IOACPIAddress address;
	unsigned int bitwidth;
};

struct PowerState
{
	bool active;
	unsigned int freq, power, tlat, bmlat, control, status, original_control;
};
static struct LogicalCPU
{
	bool active, useAMDDirectDrive, useIntelDirectDrive;
	IOACPIPlatformDevice *cpudevice;
	acpi_px_reg regcontrol, regstatus;
	PowerState powerStates[16];
	int maxPowerStates, currentState;
} logicalCPUs[8];

#ifdef DEBUG
static int verbose = 1;
#else
static int verbose = 1;
#endif
static char frequencies[1024] = "";
static uint32_t baseSystemFreq;
#define dbg if(verbose) printf


/**********************************************************************/

#define ERRH(r) { IOReturn __r=r; if(kIOReturnSuccess!=__r) { dbg("ACPICPUThrottle: Failed with code %d\n", __r); return false; } }

#define dbgPEClock(v) dbg("ACPICPUThrottle:    " #v "=%llu\n", gPEClockFrequencyInfo. v )
#define abs(v) ((((long)(v))<0) ? -(long)(v) : (long)(v))

// Kernel exports we need but aren't declared in the public header files
__BEGIN_DECLS
extern void rtc_clock_stepping(uint32_t new_frequency, uint32_t old_frequency);
extern void rtc_clock_stepped(uint32_t new_frequency, uint32_t old_frequency);
__END_DECLS

static void printPEClockFrequencyInfo()
{
	dbgPEClock(bus_clock_rate_hz);
	dbgPEClock(cpu_clock_rate_hz);
	dbgPEClock(dec_clock_rate_hz);
	dbgPEClock(bus_clock_rate_num);
	dbgPEClock(bus_clock_rate_den);
	dbgPEClock(bus_to_cpu_rate_num);
	dbgPEClock(bus_to_cpu_rate_den);
	dbgPEClock(bus_to_dec_rate_num);
	dbgPEClock(bus_to_dec_rate_den);
	dbgPEClock(timebase_frequency_hz);
	dbgPEClock(timebase_frequency_num);
	dbgPEClock(timebase_frequency_den);
	dbgPEClock(bus_frequency_hz);
	dbgPEClock(cpu_frequency_hz);
}

static void fixupPEClockFrequencyInfo(LogicalCPU &lc)
{
	// Setup gPEClockFrequencyInfo which seems to have mostly random values
	gPEClockFrequencyInfo.timebase_frequency_hz = 1000000000;
	gPEClockFrequencyInfo.bus_frequency_hz      =  100000000;
	gPEClockFrequencyInfo.bus_frequency_min_hz = gPEClockFrequencyInfo.bus_frequency_hz;
	gPEClockFrequencyInfo.bus_frequency_max_hz = gPEClockFrequencyInfo.bus_frequency_hz;
	gPEClockFrequencyInfo.cpu_frequency_min_hz=lc.powerStates[lc.maxPowerStates-1].freq*1000000ULL;
	gPEClockFrequencyInfo.cpu_frequency_max_hz=lc.powerStates[0].freq*1000000ULL;
	gPEClockFrequencyInfo.cpu_frequency_hz=lc.powerStates[lc.currentState].freq*1000000ULL;
	gPEClockFrequencyInfo.dec_clock_rate_hz = gPEClockFrequencyInfo.timebase_frequency_hz;
	gPEClockFrequencyInfo.bus_clock_rate_hz = gPEClockFrequencyInfo.bus_frequency_hz;
	gPEClockFrequencyInfo.cpu_clock_rate_hz = gPEClockFrequencyInfo.cpu_frequency_hz;
	gPEClockFrequencyInfo.bus_clock_rate_num = gPEClockFrequencyInfo.bus_clock_rate_hz;
	gPEClockFrequencyInfo.bus_clock_rate_den = 1;
	gPEClockFrequencyInfo.bus_to_cpu_rate_num = (2 * gPEClockFrequencyInfo.cpu_clock_rate_hz) / gPEClockFrequencyInfo.bus_clock_rate_hz;
	gPEClockFrequencyInfo.bus_to_cpu_rate_den = 2;
	gPEClockFrequencyInfo.bus_to_dec_rate_num = 1;
	gPEClockFrequencyInfo.bus_to_dec_rate_den = gPEClockFrequencyInfo.bus_clock_rate_hz / gPEClockFrequencyInfo.dec_clock_rate_hz;
	//dbg("ACPICPUThrottle: gPEClockFrequencyInfo after fixup:\n");
	//printPEClockFrequencyInfo();
}

static uint32_t determineRTCClockStepping(uint32_t newfreq, uint32_t basefreq)
{
	uint64_t rtc_cycle_count=(basefreq*1000000ULL)/20;
	uint64_t newcount=rtc_cycle_count*newfreq/basefreq;
	dbg("ACPICPUThrottle: rtc_cycle_count=%llu, newcount=%llu, cutoff=%llu\n", rtc_cycle_count, newcount, (1000000000ULL/20));
    //return newcount;
	if(newcount>(1000000000ULL/20))
	{	// It uses quick algorithm (which works)
		return newfreq;
	}
	else
	{	// It uses slow algorithm (which does not work). Return the slowest the system can count.
		return 1000;
	}
}

static bool setCPU(LogicalCPU &lc, int powerstate)
{
	dbg("ACPICPUThrottle: Setting %s to powerstate %d\n", lc.cpudevice->getName(), powerstate);
	if(powerstate<0) powerstate=0;
	if(powerstate>=lc.maxPowerStates) powerstate=lc.maxPowerStates-1;
	//if(powerstate==lc.currentState) return true;
	fixupPEClockFrequencyInfo(lc);
	rtc_clock_stepping(determineRTCClockStepping(lc.powerStates[powerstate].freq, lc.powerStates[0].freq), baseSystemFreq);
	if(lc.regcontrol.bitwidth)
	{
		dbg("ACPICPUThrottle: Writing 0x%x width %u to 0x%llx\n", lc.powerStates[powerstate].control, lc.regcontrol.bitwidth, lc.regcontrol.address.addr64);
		ERRH(lc.cpudevice->writeAddressSpace(lc.powerStates[powerstate].control, kIOACPIIORange,
											 lc.regcontrol.address, lc.regcontrol.bitwidth));
		UInt64 status;
		for(int n=0; n<100; n++)
		{
			status=0;
			ERRH(lc.cpudevice->readAddressSpace(&status, kIOACPIIORange,
												lc.regstatus.address, lc.regstatus.bitwidth));
			if(status==lc.powerStates[powerstate].status)
			{
				dbg("ACPICPUThrottle: Throttle succeeded!\n");
				rtc_clock_stepped(determineRTCClockStepping(lc.powerStates[powerstate].freq, lc.powerStates[0].freq), baseSystemFreq);
				lc.currentState=powerstate;
				fixupPEClockFrequencyInfo(lc);
				return true;
			}
			IODelay(10);
		}
		printf("ACPICPUThrottle: Throttle failed with status=0x%llx!\n", status);
	}
	else if(lc.useAMDDirectDrive)
	{	// Direct drive. Copied straight from the FreeBSD driver.
#if defined(__i386__) || defined(__amd64__)
		int state;
		u_int irt, rvo, pll, mvs, vst, vid, fid;
		u_int val, rvo_steps, cur_vid, cur_fid, vco_fid, vco_cur_fid, vco_diff;
		u_int64_t v64;
		state = powerstate;
			
		v64 = lc.powerStates[state].control;
		irt = control_irt(v64);
		rvo = control_rvo(v64);
		pll = control_pll(v64);
		mvs = control_mvs(v64);
		vst = control_vst(v64);
		vid = control_vid(v64);
		fid = control_fid(v64);
			
		READ_PENDING_WAIT(v64);
		cur_vid = status_vid(v64);
		cur_fid = status_fid(v64);
		dbg("ACPICPUThrottle: Phase 1: cur_vid=0x%x, cur_fid=0x%x, wantvid=0x%x, wantfid=0x%x\n", cur_vid, cur_fid, vid, fid);
				
		/* Phase 1 */
		while (cur_vid > vid) {
			val = cur_vid - (1 << mvs);
			write_fidvid(cur_fid, (val > 0) ? val : 0, 1ULL);
			READ_PENDING_WAIT(v64);
			cur_vid = status_vid(v64);
			count_off_vst(vst);
		}
			
		for (rvo_steps = rvo; rvo_steps > 0 && cur_vid > 0; rvo_steps--) {
			write_fidvid(cur_fid, cur_vid - 1, 1ULL);
			READ_PENDING_WAIT(v64);
			cur_vid = status_vid(v64);
			count_off_vst(vst);
		}
		dbg("ACPICPUThrottle: Phase 2: cur_vid=0x%x, cur_fid=0x%x, wantvid=0x%x, wantfid=0x%x\n", cur_vid, cur_fid, vid, fid);
			
		/* Phase 2 */
		if (cur_fid != fid) {
			vco_fid = FID_TO_VCO_FID(fid);
			vco_cur_fid = FID_TO_VCO_FID(cur_fid);
			vco_diff = (vco_cur_fid < vco_fid) ?
				vco_fid - vco_cur_fid : vco_cur_fid - vco_fid;
			while (vco_diff > 2) {
				if (fid > cur_fid) {
					if (cur_fid > 6)
						val = cur_fid + 2;
					else
						val = FID_TO_VCO_FID(cur_fid) + 2;
				} else {
					val = cur_fid - 2;
				}
				write_fidvid(val, cur_vid, pll * 200ULL);
				READ_PENDING_WAIT(v64);
				cur_fid = status_fid(v64);
				count_off_irt(irt);
					
				vco_cur_fid = FID_TO_VCO_FID(cur_fid);
				vco_diff = (vco_cur_fid < vco_fid) ?
					vco_fid - vco_cur_fid : vco_cur_fid - vco_fid;
			}
				
			write_fidvid(fid, cur_vid, pll * 200ULL);
			READ_PENDING_WAIT(v64);
			cur_fid = status_fid(v64);
			count_off_irt(irt);
		}
		dbg("ACPICPUThrottle: Phase 3: cur_vid=0x%x, cur_fid=0x%x, wantvid=0x%x, wantfid=0x%x\n", cur_vid, cur_fid, vid, fid);
			
		/* Phase 3 */
		if (cur_vid != vid) {
			write_fidvid(cur_fid, vid, 1ULL);
			READ_PENDING_WAIT(v64);
			cur_vid = status_vid(v64);
			count_off_vst(vst);
		}
		dbg("ACPICPUThrottle: End    : cur_vid=0x%x, cur_fid=0x%x, wantvid=0x%x, wantfid=0x%x\n", cur_vid, cur_fid, vid, fid);
			
		/* Done */
		if (cur_vid == vid && cur_fid == fid)
		{
			dbg("ACPICPUThrottle: Throttle succeeded with cur_vid=0x%x, cur_fid=0x%x, wantvid=0x%x, wantfid=0x%x!\n", cur_vid, cur_fid, vid, fid);
			rtc_clock_stepped(determineRTCClockStepping(lc.powerStates[powerstate].freq, lc.powerStates[0].freq), baseSystemFreq);
			lc.currentState=powerstate;
			fixupPEClockFrequencyInfo(lc);
			return true;
		}
		printf("ACPICPUThrottle: Throttle failed with cur_vid=0x%x, cur_fid=0x%x, wantvid=0x%x, wantfid=0x%x!\n", cur_vid, cur_fid, vid, fid);
#endif	/* __i386__ || __amd64__ */
	}
	else if(lc.useIntelDirectDrive)
	{
#if defined(__i386__) || defined(__amd64__)
		// Copied straight from the EnhancedSpeedStep driver
		uint64_t msr, v64 = lc.powerStates[powerstate].control;
		msr = rdmsr64(MSR_PERF_CTL);
		msr = (msr & ~(uint64_t)(0xffff)) | v64;
		wrmsr64(MSR_PERF_CTL, msr);
        dbg("ACPICPUThrottle: status = %x", lc.powerStates[powerstate].status);
		unsigned int inc=lc.powerStates[powerstate].tlat/10;
		for(unsigned int n=0; n<=lc.powerStates[powerstate].tlat; n+=inc)
		{
			IODelay(inc);
			msr = rdmsr64(MSR_PERF_STATUS) & 0xffff;
			if(msr == lc.powerStates[powerstate].status)
				break;
		}
		if(msr == lc.powerStates[powerstate].control)
		{
			dbg("ACPICPUThrottle: Throttle succeeded with cur_status=0x%x, want_status=0x%x!\n", msr, lc.powerStates[powerstate].status);
			rtc_clock_stepped(determineRTCClockStepping(lc.powerStates[powerstate].freq, lc.powerStates[0].freq), baseSystemFreq);
			lc.currentState=powerstate;
			fixupPEClockFrequencyInfo(lc);
			return true;
		}
		printf("ACPICPUThrottle: Throttle failed with cur_status=0x%x, want_status=0x%x!\n", msr, lc.powerStates[powerstate].status);
#endif	/* __i386__ || __amd64__ */
	}
	return false;
}
static bool setCPUs(int powerstate)
{
	bool ret=true;
	for(int n=0; logicalCPUs[n].active; n++)
	{
		ret=ret && setCPU(logicalCPUs[n], powerstate);
	}
	return ret;
}

static bool addCPU(LogicalCPU &lc, IOACPIPlatformDevice *cpu)
{
	OSObject *result;
	printf("ACPICPUThrottle: Adding %s\n", cpu->getName());
	lc.cpudevice=cpu;
	
	// Fetch 8.3.3.1 _PCT, returns OSArray
	ERRH(cpu->evaluateObject("_PCT", &result));
	if(!result)
	{
		printf("ACPICPUThrottle: This ACPI does not permit CPU throttling!\n");
		return false;
	}
	OSObjPtr perfCtrl(result);			// Actually an OSArray
	
	// Get Performance Control Register
	result=((OSArray *) *perfCtrl)->getObject(0);
	if(!result) return false;
	{	// Now result points to type OSData
		OSData *d=(OSData *) result;
		const ACPIResource *r=(const ACPIResource *) d->getBytesNoCopy();
		dbg("ACPICPUThrottle: type=%x, datalen=%u, _ASI=%x, _RBW=%x, _RBO=%x, _ADR=%x\n",
			r->type, r->dataitemlen, r->data.genericReg._ASI, r->data.genericReg._RBW, r->data.genericReg._RBO, r->data.genericReg._ADR);
		if(!r || /* generic register */0x82!=r->type)
			return false;
		if(/* functional fixed hardware */0x7f==r->data.genericReg._ASI)
		{	// Must direct drive
			lc.regcontrol.bitwidth=0;
			dbg("ACPICPUThrottle: ACPI returns no ACPI throttling available\n");
		}
		else
		{
			memcpy(&lc.regcontrol.address.addr64, &r->data.genericReg._ADR.addr64, 8);
			memcpy(&lc.regcontrol.bitwidth, &r->data.genericReg._RBW, 2);
			dbg("ACPICPUThrottle: PCR=0x%x width %u\n", lc.regcontrol.address.addr64, lc.regcontrol.bitwidth);
		}
	}
	
	// Get Performance Status Register
	result=((OSArray *) *perfCtrl)->getObject(1);
	if(!result) return false;
	{	// Now result points to type OSData
		OSData *d=(OSData *) result;
		const ACPIResource *r=(const ACPIResource *) d->getBytesNoCopy();
		dbg("ACPICPUThrottle: type=%x, datalen=%u, _ASI=%x, _RBW=%x, _RBO=%x, _ADR=%x\n",
			r->type, r->dataitemlen, r->data.genericReg._ASI, r->data.genericReg._RBW, r->data.genericReg._RBO, r->data.genericReg._ADR);
		if(!r || 0x82!=r->type) return false;
		if(/* functional fixed hardware */0x7f==r->data.genericReg._ASI)
		{	// Must direct drive
			lc.regcontrol.bitwidth=0;
			dbg("ACPICPUThrottle: ACPI returns no ACPI throttling available\n");
		}
		else
		{
			memcpy(&lc.regstatus.address.addr64, &r->data.genericReg._ADR.addr64, 8);
			memcpy(&lc.regstatus.bitwidth, &r->data.genericReg._RBW, 2);
			dbg("ACPICPUThrottle: PSR=0x%x width %u\n", lc.regstatus.address.addr64, lc.regstatus.bitwidth);
		}
	}

	// Fetch 8.3.3.2 _PSS, returns OSArray
	ERRH(cpu->evaluateObject("_PSS", &result));
	if(!result) return false;
	OSObjPtr perfStates(result);			// Actually an OSArray
	OSArray *_perfStates=(OSArray *) *perfStates;
	lc.maxPowerStates=_perfStates->getCount();
	if(lc.maxPowerStates>sizeof(lc.powerStates)/sizeof(lc.powerStates[0]))
		lc.maxPowerStates=sizeof(lc.powerStates)/sizeof(lc.powerStates[0]);
	for(int n=0; n<lc.maxPowerStates; n++)
	{
		PowerState &powerState=lc.powerStates[n];
		OSArray *ps=(OSArray *) _perfStates->getObject(n);
		powerState.freq   =((OSNumber *) ps->getObject(0))->unsigned32BitValue();
		powerState.power  =((OSNumber *) ps->getObject(1))->unsigned32BitValue();
		powerState.tlat   =((OSNumber *) ps->getObject(2))->unsigned32BitValue();
		powerState.bmlat  =((OSNumber *) ps->getObject(3))->unsigned32BitValue();
		powerState.control=((OSNumber *) ps->getObject(4))->unsigned32BitValue();
        powerState.original_control=powerState.control;
		powerState.status =((OSNumber *) ps->getObject(5))->unsigned32BitValue();
		
		if(!powerState.freq)
		{
			lc.maxPowerStates=n;
			break;
		}
		powerState.active=true;
	}
	// Now cull duplicates
	for(int n=lc.maxPowerStates-1; n>0; n--)
	{
		if(lc.powerStates[n].control==lc.powerStates[n-1].control)
		{
			memmove(&lc.powerStates[n-1], &lc.powerStates[n], (lc.maxPowerStates-n)*sizeof(PowerState));
			lc.maxPowerStates--;
		}
	}
	// Generate sysctl list
	char *sysctlend=frequencies;
	int lastActive=lc.maxPowerStates;
	for(int n=lc.maxPowerStates-1; n>=0; n--)
	{
		PowerState &powerState=lc.powerStates[n];
		if(powerState.freq<=1000)
		{
			lastActive=n;
			powerState.active=false;
		}
		else
		{
			sprintf(sysctlend, "%d ", lc.powerStates[n].freq);
			sysctlend=strchr(sysctlend, 0);
		}
	}
	*sysctlend++=10;
	strcpy(sysctlend, "ACPICPUThrottle v1.1.0 (" __DATE__ ") (C) 2006 Niall Douglas\n"
					  "(C) 2008 Prashant Vaibhav <mercurysquad@yahoo.com>\n");
	sysctlend=strchr(sysctlend, 0);
	for(int n=0; n<lc.maxPowerStates; n++)
	{
		PowerState &powerState=lc.powerStates[n];
		if(!powerState.active)
			sprintf(sysctlend, "P%u, %uMhz disabled\n", n, powerState.freq);
		else
			sprintf(sysctlend, "P%u, %uMhz, %umW, %uus, %uus (ctrl=0x%x, status=0x%x)\n", n,
					powerState.freq, powerState.power, powerState.tlat, powerState.bmlat,
					powerState.control, powerState.status);
		printf("---> %s", sysctlend);
		sysctlend=strchr(sysctlend, 0);
	}
	strcpy(sysctlend, "(CPU Powerstates under 1GHz are not available on MacOS X)\n");
	lc.maxPowerStates=lastActive;
	if(1==lc.maxPowerStates)
	{
		printf("ACPICPUThrottle: Only one available power state found (all states <=1Ghz are unavailable), exiting!\n");
		return false;
	}
#if 0
	printf("ACPICPUThrottle: DEBUG VERSION FORCING DIRECT DRIVE!\n");
	lc.regcontrol.bitwidth=0;
#endif
	if(lc.regcontrol.bitwidth)
	{
		printf("ACPICPUThrottle: Using ACPI BIOS to throttle CPU\n");
	}
	else
	{	// There are no ACPI throttle control registers. Must therefore direct drive if possible
#if defined(__i386__) || defined(__amd64__)
		i386_cpu_info_t *info = cpuid_info();
		if (strncmp(info->cpuid_vendor, "AuthenticAMD", 12) == 0) {
			dbg("ACPICPUThrottle: AMD processor detected, family=%x, model=%x\n", info->cpuid_family, info->cpuid_model);
			if(0xF==info->cpuid_family && info->cpuid_model>=0x4)
			{
				u_int regs[4];
				do_cpuid(0x80000000, regs);
				if (regs[0] < 0x80000007)	/* EAX[31:0] */
					return false;
				do_cpuid(0x80000007, regs);
				if ((regs[3] & 0x6) != 0x6)	/* EDX[2:1] = 11b */
					return false;
				printf("ACPICPUThrottle: Using direct drive of AMD K8 throttling\n");
				lc.useAMDDirectDrive=true;
				goto allgood;
			}
		}
		if (strncmp(info->cpuid_vendor, "GenuineIntel", 12) == 0) {
			dbg("ACPICPUThrottle: Intel processor detected, family=%x, model=%x\n", info->cpuid_family, info->cpuid_model);
			if(0x6==info->cpuid_family || 0xF==info->cpuid_family)
			{
				u_int regs[4];
				do_cpuid(1, regs);
				if ((regs[2] & 0x80) == 0)
					return false;
				printf("ACPICPUThrottle: Using direct drive of Intel throttling\n");
				lc.useIntelDirectDrive=true;
				goto allgood;
			}
		}
#endif	/* __i386__ || __amd64__ */
		printf("ACPICPUThrottle: No method available to throttle CPU, exiting!\n");
		return false;
	}
allgood:
	// Determine what the system is currently running at
	lc.currentState=-1;
	uint32_t diff=1<<30;
	for(int n=0; n<sizeof(lc.powerStates)/sizeof(lc.powerStates[0]); n++)
	{
		PowerState &powerState=lc.powerStates[n];
		//printf("diff=%ld\n", abs(powerState.freq-gPEClockFrequencyInfo.cpu_frequency_hz/1000000ULL));
		if(powerState.freq && abs(powerState.freq-gPEClockFrequencyInfo.cpu_frequency_hz/1000000ULL)<diff)
		{
			lc.currentState=n;
			baseSystemFreq=powerState.freq;
			diff=abs(powerState.freq-gPEClockFrequencyInfo.cpu_frequency_hz/1000000ULL);
		}
	}
	if(diff>=50) lc.currentState=-1;
	if(-1==lc.currentState)
	{
		printf("ACPICPUThrottle: Failed to determine current system speed (no match to %llu)!\n", gPEClockFrequencyInfo.cpu_frequency_hz/1000000ULL);
		return false;
	}
		
	if(!setCPU(lc, lc.maxPowerStates-1))
	{	// Failed to set, so don't activate
		return true;
	}

	//dbg("ACPICPUThrottle: first item is metaclass of %s\n", result->getMetaClass()->getClassName());
	
	lc.active=true;
	return true;
}

int find_closest_pstate(int MHz_wanted) {
    // finds the P-state which has a frequency closest to the supplied frequency
    int pstate, bestdiff=1<<30;
    for(int n=0; n<logicalCPUs[0].maxPowerStates; n++)
    {
        PowerState &ps=logicalCPUs[0].powerStates[n];
        if(abs(MHz_wanted-(int) ps.freq)<bestdiff)
        {
            pstate=n;
            bestdiff=abs(MHz_wanted-(int) ps.freq);
        }
    }
    return pstate;
}


/**********************************************************************/

static int cputhrottle_sysctl_mhz SYSCTL_HANDLER_ARGS
{
	int err=0;
	if(req->newptr)
	{
		int pstate, MHz_wanted;
		err = SYSCTL_IN(req, &MHz_wanted, sizeof(int));
		if (err)
			return err;
		if(MHz_wanted<16)
		{	// pstate specified directly
			pstate=MHz_wanted;
		}
		else
		{   // freq in mhz is given, find closest pstate
            pstate = find_closest_pstate(MHz_wanted);
		}
		setCPUs(pstate);
	}
	else
	{
		int MHz=logicalCPUs[0].powerStates[logicalCPUs[0].currentState].freq;
		err = SYSCTL_OUT(req, &MHz, sizeof(int));
	}
	return err;
}

void setFIDVID(int FID_wanted, int VID_wanted, bool setCPUAlso) {
    // modifies the "control" parameter of a pstate to set the given VID for this FID
    // if setCPUAlso is true, it will also immediately update the current CPU state
    // find the pstate for this FID.
    for (int i = 0; i < logicalCPUs[0].maxPowerStates; i++) {
        PowerState &ps = logicalCPUs[0].powerStates[i];
        if (FID_wanted == (ps.control & INTEL_MSR_FID_MASK)) {
            // found the proper pstate, now update its voltage if it's not higher than default
            if (VID_wanted <= (ps.original_control & INTEL_MSR_VID_MASK))
            {   // this is fine to set.
                ps.control = (FID_wanted | VID_wanted);
                // and now actually set the CPUs
                dbg("ACPICPUThrottle: FID=%u being set to VID=%u, status=%x\n", FID_wanted >> 8, VID_wanted, ps.status);
                if (setCPUAlso) setCPUs(find_closest_pstate(ps.freq));
                break;
            } else {
                // this is higher than default, we won't change it
                dbg("ACPICPUThrottle: VID=%u is higher than default, not changing!\n", VID_wanted);
            }
        }
    }
    
}

static int cputhrottle_sysctl_vid SYSCTL_HANDLER_ARGS
{
    // For reading/writing the voltage multiplier sysctl on intel
    if (!logicalCPUs[0].useIntelDirectDrive)
        return -1; // return with error if it's not intel
    
	int err=0;
	if(req->newptr)
	{
        // to set a FID/VID
        int CTL_wanted = 0, FID_wanted = 0, VID_wanted;
        err = SYSCTL_IN(req, &CTL_wanted, sizeof(int));
        FID_wanted = CTL_wanted & INTEL_MSR_FID_MASK;
        VID_wanted = CTL_wanted & INTEL_MSR_VID_MASK;
        setFIDVID(FID_wanted, VID_wanted, true);
	}
	else
	{
        // to get the current FID/VID of the first logical processor
        int FIDVID=logicalCPUs[0].powerStates[logicalCPUs[0].currentState].control;
		err = SYSCTL_OUT(req, &FIDVID, sizeof(int));
	}
	return err;
}

/*void load_vidsettings_from_plist() {
    // this tries to load any VID settings for the pstates by looking in the Info.plist file
    OSArray* vids = (OSArray*) getProperty("VIDSettings");
    if (!vids) {
        dbg("ACPICPUThrottle: No VIDs specified in Info.plist, using safe factory default voltages.\n");
        return;
    }
    
    int FID, VID;
    
    for (int pstate = 0; pstate < (vids->getCount()); pstate++) {
        uint32_t voltage = ((OSNumber*) vids->getObject(pstate))->unsigned32BitValue();
        if (!voltage) continue; // if no valid number specified, move on
        FID = (logicalCPUs[0].powerStates[pstate].control) & INTEL_MSR_FID_MASK;
        VID = (voltage - 700) / 16; // formula to get VID from voltage [for intel cpus]
        setFIDVID(FID, VID, false);
    }
}*/

SYSCTL_INT(_kern, OID_AUTO, cputhrottle_verbose, CTLFLAG_RW, &verbose,
		   0, "Log CPU frequency changes");
SYSCTL_PROC(_kern, OID_AUTO, cputhrottle_curfreq, CTLTYPE_INT | CTLFLAG_RW, 0, 0,
			&cputhrottle_sysctl_mhz, "I", "Current CPU frequency");
SYSCTL_PROC(_kern, OID_AUTO, cputhrottle_vid    , CTLTYPE_INT | CTLFLAG_RW, 0, 0,
            &cputhrottle_sysctl_vid, "I", "Current CPU voltage multiplier");
SYSCTL_STRING(_kern, OID_AUTO, cputhrottle_freqs, CTLFLAG_RD, frequencies, 0,
			  "CPU frequencies supported");

extern "C" kern_return_t ACPICPUThrottle_start (kmod_info_t * ki, void * d) {
	dbg("ACPI CPU Throttle loaded\n");
	//printPEClockFrequencyInfo();
	const IORegistryPlane *IODeviceTree=IORegistryEntry::getPlane("IODeviceTree");
	if(!IODeviceTree) return KERN_FAILURE;
	
	// Firstly, let's see what CPUs we have
	IORegPtr cpus(IORegistryEntry::fromPath("/cpus", IODeviceTree));
	if(!cpus) return KERN_FAILURE;
	IORegistryEntry *cpu;
	int n=0, i=0;
	for(IORegIt it(cpus, IODeviceTree); cpu=it->getCurrentEntry(); it->getNextObjectFlat(), n++)
	{	// Sadly OSDynamicCast is not available to us
		if(n && i<sizeof(logicalCPUs)/sizeof(LogicalCPU))
		{
			if(addCPU(logicalCPUs[i], static_cast<IOACPIPlatformDevice *>(cpu)))
				i++;
		}
	}
	if(!logicalCPUs[0].active)
	{
		printf("ACPICPUThrottle: No valid CPUs returned by ACPI! It's possible your BIOS "
			   "does not think your system should be throttled and therefore did not "
			   "return any available power states, or maybe your CPU only supports one "
			   "other power state than maximum which is below 1Ghz!\n");
		return KERN_FAILURE;
	}

	/* Register our SYSCTL */
	sysctl_register_oid(&sysctl__kern_cputhrottle_curfreq); 
	sysctl_register_oid(&sysctl__kern_cputhrottle_verbose); 
	sysctl_register_oid(&sysctl__kern_cputhrottle_freqs);
	sysctl_register_oid(&sysctl__kern_cputhrottle_vid);
    dbg("ACPICPUThrottle: Loading voltage values from Info.plist\n");
    /*load_vidsettings_from_plist();*/
    return KERN_SUCCESS;
}


extern "C" kern_return_t ACPICPUThrottle_stop (kmod_info_t * ki, void * d) {
	setCPUs(0);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_curfreq); 
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_verbose); 
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_freqs);
    sysctl_unregister_oid(&sysctl__kern_cputhrottle_vid);
	dbg("ACPI CPU Throttle unloaded\n");
    return KERN_SUCCESS;
}
