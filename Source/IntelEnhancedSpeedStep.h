#ifndef _INTELENHANCEDSPEEDSTEP_H
#define _INTELENHANCEDSPEEDSTEP_H

#include <IOKit/IOService.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include "IOCPU.h" // This is not in Kernel IOKit framework, so have to redefine.

#include <i386/proc_reg.h>
#include <i386/cpuid.h>
#include <i386/proc_reg.h>

#include <sys/sysctl.h>
#include <sys/types.h>

#include <kern/host.h>

#include <mach/mach_interface.h>
#include <mach/mach_vm.h>
#include <mach/mach_host.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include <mach/processor.h>
#include <mach/processor_info.h>

/*
 * This class holds information about each throttle state
 */
class PState {
public:
	uint16_t Frequency;		// processor clock speed (FID, not MHz)
uint16_t AcpiFreq;		// as reported by ACPI (nice rounded) for display purposes
	uint16_t Voltage;		// wanted voltage ID while on AC
	uint16_t OriginalVoltage;	// The factory default voltage ID for this frequency
	uint32_t Latency;		// how long to wait after writing to msr
	uint64_t TimesChosen;		// how many times this Pstate was chosen to switch to
};


/*
 * Our auto-throttle controller
 */
class AutoThrottler : public OSObject {
OSDeclareDefaultStructors(AutoThrottler)
private:
	IOWorkLoop*		workLoop;
	IOTimerEventSource*	perfTimer;
	bool			enabled;	// driver is autothrottling
	uint8_t			currentPState;
	uint64_t		lastTime;
	host_t			selfHost;
#define max_cpus 32
	processor_t		mach_cpu[max_cpus];
	uint8_t			cpu_count;
	uint32_t		total_ticks[max_cpus];
	uint32_t		load_ticks[max_cpus];
	uint32_t		current_cpuload;
	processor_cpu_load_info	cpu_load[max_cpus];
	processor_cpu_load_info	cpu_load_temp[max_cpus];
	processor_cpu_load_info	cpu_load_last[max_cpus];

public:
	bool setupDone;	// setup has been done, ready to throttle
	uint16_t targetCPULoad;
	bool setup(OSObject* owner); // Initialize the throttler
	void stop();
	void setEnabled(bool _enabled);
	bool getEnabled();
	void destruct();
	
	void GetCPUTicks(long* idle, long* total);
	bool perfTimerEvent(IOTimerEventSource* src, int count);
};

const uint32_t throttleQuantum		= 100; // ms
const uint32_t defaultTargetLoad	= 400; // percent x 10

bool perfTimerWrapper(OSObject* owner, IOTimerEventSource* src, int count);

/*********************************************************************************************************
/*
 * The following is used with mp_rendezvous to throttle all CPUs
 */
void disableInterrupts	(__unused void* t);
void enableInterrupts	(__unused void* t);
void throttleCPU	(void* fidvid);

/*
 * Rendezvous
 */
extern "C" void mp_rendezvous(	void (*setup_func) (void *),
			void (*action_func) (void *),
			void (*teardown_func) (void *),
			void *arg);

/*
 * For timer fix
 */
__BEGIN_DECLS
/* The next 2 functions are used for clock recalibration on non-constant tsc */
extern void rtc_clock_stepping(uint32_t new_frequency, uint32_t old_frequency);
extern void rtc_clock_stepped (uint32_t new_frequency, uint32_t old_frequency);
/* The following is our magic function for kernel feature autodetect */
extern void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
__END_DECLS

/*
 * The main throttling function. This sets up mp_rendezvous and provides
 * the proper fid/vid for the given P-State.
 */
void throttleAllCPUs(PState* p);

/*
 * Gets the current core voltage. Only current processor is read
 */
uint16_t getCurrentVoltage();

/*
 * Returns the current operating frequency in MHz
 */
uint16_t getCurrentFrequency();

/*
 * Check heuristically whether constant_tsc is supported
 */
bool isConstantTSC();

/*
 * Check whether CPU supports N/2 fsb ratios
 */
void checkForNby2Ratio();

/*
 * Create the PState table by getting info from ACPI
 */
bool createPStateTable(PState* pS, unsigned int* numStates);
/*
 * Gets the FSB frequency from EFI
 */
bool getFSB();

/*
 * Loads override PState table from the Info.plist's array
 */
void loadPStateOverride(OSArray* dict);

/*
 * Convert VID to mV
 */
uint16_t VID_to_mV(uint8_t VID);

/*
 * Convert mV to VID
 */
uint8_t mV_to_VID(uint16_t mv);

/*
 * FID to Mhz and vice versa
 */
uint8_t  MHz_to_FID(uint16_t mhz);
uint16_t FID_to_MHz(uint8_t fid);

/*
 * Check if we are running on newer core2duo
 */
void checkForPenryn();

/* Sysctl stuff */
char*	getFreqList();
char*	getVoltageList(bool original);
int	FindClosestPState(int wantedFreq);
char	frequencyList		[1024] = "";
char	originalVoltages	[1024] = "";
uint64_t totalThrottles, totalTimerEvents;
char	frequencyUsage		[1024] = "";


/*
 * Certain (pseudo)global variables
 */
PState		PStates[16];		// 16 states max
AutoThrottler*	Throttler;		// Our autothrottle controller
unsigned int	NumberOfPStates;	// How many this processor supports
IOSimpleLock*	Lock;			// lock to use while throttling
bool		InterruptsEnabled;	// to save state of interrupts before throttling
bool		Is45nmPenryn;		// so that we can use proper VID -> mV calculation
bool		RtcFixKernel;		// to indicate if this kernel has rtc fix
bool		Below1Ghz;		// whether kernel is patched to support < 1Ghz freqs
bool		ConstantTSC;		// whether processor supports constant tsc
bool		Nby2Ratio;		// Whether cpu supports N/2 fsb ratio
bool		DebugOn;		// whether to print debug messages
uint64_t	FSB;			// as reported by EFI
uint32_t	MaxLatency;		// how long to wait after switching pstate
int		DefaultPState;		// set at startup
int		NumberOfProcessors;	// # of cores/ACPI cpus actually
/*
 * The IOKit driver class
 */
class net_mercurysquad_driver_IntelEnhancedSpeedStep : public IOService {
OSDeclareDefaultStructors(net_mercurysquad_driver_IntelEnhancedSpeedStep)
	
private:
	IOWorkLoop*	workLoop;

public:
	virtual bool		init	(OSDictionary *dictionary = 0);
	virtual void		free	(void);
	virtual IOService*	probe	(IOService *provider, SInt32 *score);
	virtual bool		start	(IOService *provider);
	virtual void		stop	(IOService *provider);
};

#endif // _INTELENHANCEDSPEEDSTEP_H