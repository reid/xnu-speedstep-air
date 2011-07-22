extern "C" {
#include <pexpert/pexpert.h>
}

#define super IOService

#include "IntelEnhancedSpeedStep.h"
#include "Utility.h"


OSDefineMetaClassAndStructors(com_reidburke_air_IntelEnhancedSpeedStep, IOService)
OSDefineMetaClassAndStructors(AutoThrottler, OSObject)

/**********************************************************************************/
/* sysctl interface for compatibility with Niall Douglas' ACPICPUThrottle.kext    */

static int iess_handle_curfreq SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (req->newptr) {
		// New freq being set
		int pstate, wantedFreq;
		err = SYSCTL_IN(req, &wantedFreq, sizeof(int));
		if (err) return err;
		
		if (wantedFreq < 16 && wantedFreq < NumberOfPStates) // pstate specified directly
			pstate = wantedFreq;
		else // freq in MHz is given, find closest pstate
			pstate = FindClosestPState(wantedFreq);
		
		dbg("Throttling to PState %d\n", pstate);
		throttleAllCPUs(&PStates[pstate]);

	} else { // just reading
		int MHz = PStates[FindClosestPState(getCurrentFrequency())].AcpiFreq;
		err = SYSCTL_OUT(req, &MHz, sizeof(int));
	}
	
	return err;
}

static int iess_handle_curvolt SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (req->newptr) {
		// New voltage being set
		int wantedvolt;
		err = SYSCTL_IN(req, &wantedvolt, sizeof(int));
		if (err) return err;
		int pstate   = FindClosestPState(getCurrentFrequency());
		int origvolt = VID_to_mV(PStates[pstate].OriginalVoltage);
		if (wantedvolt > origvolt+100) {
			wantedvolt = origvolt;
			warn("Will not set voltage more than 100 mV higher than factory spec %d mV\n", origvolt);
		}
		dbg("Changing voltage of current PState %d to %d mV\n", pstate, wantedvolt);
		PStates[pstate].Voltage = mV_to_VID(wantedvolt);
		throttleAllCPUs(&PStates[pstate]);
	
	} else { // just reading
		int volt = getCurrentVoltage();
		err = SYSCTL_OUT(req, &volt, sizeof(int));
	}
	
	return err;
}

static int iess_handle_ctl SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (req->newptr) {
		// new ctl being set
		warn("Use of kern.cputhrottle_ctl is only for debugging."
		     "Use wisely - no validation is done!\n");
		int ctl;
		err = SYSCTL_IN(req, &ctl, sizeof(int));
		if (err) return err;
		dbg("Manual stepping to %xh\n", ctl);
		
		PState p; p.Frequency = FID(ctl); p.Voltage = VID(ctl);
		throttleAllCPUs(&p);
		
	} else {
		int ctl = rdmsr64(INTEL_MSR_PERF_STS);
		ctl &= 0xffff; // only last 32 bits
		err = SYSCTL_OUT(req, &ctl, sizeof(int));
	}
	return err;
}

static int iess_handle_auto SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (req->newptr) {
		// new ctl being set
		int whetherOn;
		err = SYSCTL_IN(req, &whetherOn, sizeof(int));
		if (err) return err;
		if (!Throttler) {
			Throttler = new AutoThrottler;
			if (!Throttler) return kIOReturnError;
		}
		// If setup was not done yet, do so.
		if (Throttler->setupDone == false) {
			if (Throttler->setup((OSObject*) Throttler) == false) {
				warn("Auto-throttler could not be setup.\n");
				return kIOReturnError;
			}
		}
		dbg("Setting autothrottle to %d\n", whetherOn);
		Throttler->setEnabled(whetherOn != 0);
		
	} else {
		if (!Throttler) return kIOReturnError;
		int sts = Throttler->getEnabled() ? 1 : 0;
		err = SYSCTL_OUT(req, &sts, sizeof(int));
	}
	return err;
}

static int iess_handle_usage SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (!req->newptr) { // reading
		int curpos = 0; uint64_t pc;
		if (totalTimerEvents == 0) return kIOReturnError;
		for (int i = NumberOfPStates - 1; i >= 0; i--) {
			pc = (100 * 100 * PStates[i].TimesChosen) / totalTimerEvents;
			sprintf((frequencyUsage + curpos), "%u.%u%%   ", pc / 100, pc % 100);
			if (pc / 100 < 10)
				curpos += 6;
			else if (pc / 100 < 100)
				curpos += 7;
			else
				curpos += 8;
			if (pc % 10 == 0)
				curpos -= 1; // when it's xx.x% and not xx.xx%
		}
		frequencyUsage[curpos] = '\0';
		err = SYSCTL_OUT(req, frequencyUsage, curpos + 1);
	}
	return err;
}

static int iess_handle_avgfreq SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (!req->newptr) { // reading
		if (totalTimerEvents == 0) return kIOReturnError;
		uint64_t sum = 0; int avg = 0;
		for (int i = NumberOfPStates - 1; i >= 0; i--) {
			sum += PStates[i].TimesChosen * FID_to_MHz(PStates[i].Frequency);
		}
		avg = sum / totalTimerEvents;
		err = SYSCTL_OUT(req, &avg, sizeof(avg));
	}
	return err;
}

SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_auto,	CTLTYPE_INT | CTLFLAG_RW, 0, 0, &iess_handle_auto,    "I", "Auto-throttle status");
SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_curfreq,	CTLTYPE_INT | CTLFLAG_RW, 0, 0, &iess_handle_curfreq, "I", "Current CPU frequency");
SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_curvolt,	CTLTYPE_INT | CTLFLAG_RW, 0, 0, &iess_handle_curvolt, "I", "Current CPU voltage");
SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_ctl,		CTLTYPE_INT | CTLFLAG_RW, 0, 0, &iess_handle_ctl,     "I", "Current MSR status");
SYSCTL_STRING(_kern, OID_AUTO, cputhrottle_freqs,	CTLFLAG_RD, frequencyList, 0, "CPU frequencies supported");
SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_usage,	CTLTYPE_STRING | CTLFLAG_RD, 0, 0, &iess_handle_usage,"A", "CPU frequency usage pattern");
SYSCTL_STRING(_kern, OID_AUTO, cputhrottle_factoryvolts,CTLFLAG_RD, originalVoltages, 0, "Factory default voltages for each frequency");
SYSCTL_QUAD  (_kern, OID_AUTO, cputhrottle_totalthrottles, CTLFLAG_RD, &totalThrottles, "Total number of frequency throttles made");
SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_avgfreq,	CTLTYPE_INT | CTLFLAG_RD, 0, 0, &iess_handle_avgfreq, "I", "Weighted average frequency in MHz at which this computer stays");
/******* Helper functions for sysctl interface *********/

/* Return null terminated list of frequencies */
char* getFreqList() {
	// Makes a list of space separated frequencies supported by the CPU
	// Each frequency can occupy 4 digits, plus a space
	char* freqs = new char[5 * NumberOfPStates];
	int c = 0;
	for (int i = NumberOfPStates-1; i >= 0; i--) {
		sprintf((freqs + c), "%d ", PStates[i].AcpiFreq);
		// Advance by 5 places if freq was of 4 digits
		if (PStates[i].AcpiFreq >= 1000)
			c += 5;
		else // otherwise 4 places
			c += 4;
	}
	// Set last char (which would be a space) to null
	freqs[c] = '\0';
	return freqs;
}

/* Return null terminated list of voltages */
char* getVoltageList(bool originals) {
	char* volts = new char[5 * NumberOfPStates];
	int c = 0, svolt = 0;
	for (int i = NumberOfPStates-1; i >= 0; i--) {
		svolt = originals ? VID_to_mV(PStates[i].OriginalVoltage) : VID_to_mV(PStates[i].Voltage);
		sprintf((volts + c), "%d ", svolt);
		// Advance by 5 places if voltage was of 4 digits
		if (svolt >= 1000)
			c += 5;
		else // otherwise 4 places
			c += 4;
	}
	// Set last char (which would be a space) to null
	volts[c] = '\0';
	return volts;
}

int FindClosestPState(int wantedFreq) {
	// assume P0 is best
	int bestpstate = 0;
	int bestdiff   = abs(PStates[0].AcpiFreq - wantedFreq);
	
	// now iterate over others and find the best
	for (int i = 1; i < NumberOfPStates; i++) {
		if (abs(PStates[i].AcpiFreq - wantedFreq) < bestdiff) {
			bestpstate = i;
			bestdiff = abs(PStates[i].AcpiFreq - wantedFreq);
		}
	}
	
	return bestpstate;
}


/***************************************************************************************************/

bool com_reidburke_air_IntelEnhancedSpeedStep::init(OSDictionary* dict) {
	bool res = super::init(dict);
	info("Initializing xnu-speedstep-air\n");
	/* Allocate our spinlock for later use */
	Lock = IOSimpleLockAlloc();
	/* Check for a patched kernel which properly implements rtc_clock_stepped() */
	uint64_t magic = -1; // means autodetect
	
	OSBoolean* debugMsgs = (OSBoolean*) dict->getObject("DebugMessages");
	if (debugMsgs != 0)
		DebugOn = debugMsgs->getValue();
	else
		DebugOn = false;
	
	OSNumber* kernelFeatures = (OSNumber*) dict->getObject("KernelFeatures");
	if (kernelFeatures != 0)
		magic = kernelFeatures->unsigned8BitValue();
	
	if (magic == 255) nanoseconds_to_absolutetime(~(0), &magic); //255uint = -1 int
	
	if (magic == 1) {
		RtcFixKernel = true;
		Below1Ghz	= false;
	} else if (magic == 2) {
		RtcFixKernel = true;
		Below1Ghz	= true;
	} else if (magic == 3) {
		RtcFixKernel = false;
		Below1Ghz = true;
	} else {
		RtcFixKernel = false;
		Below1Ghz	= false;
	}
	
	checkForNby2Ratio(); // check and store in global variable before loading pstate override
	if (getFSB() == false)
		return false;
	
	uint32_t bootarg;
	OSArray* overrideTable = (OSArray*) dict->getObject("PStateTable");
	if (overrideTable != 0 && !PE_parse_boot_arg("-autopstates", &bootarg))
		loadPStateOverride(overrideTable);
	
	OSNumber* defaultState = (OSNumber*) dict->getObject("DefaultPState");
	if (defaultState != 0)
		DefaultPState = defaultState->unsigned8BitValue();
	else
		DefaultPState = -1; // indicate no default state

	OSNumber* maxLatency = (OSNumber*) dict->getObject("Latency");
	if (maxLatency != 0)
		MaxLatency = maxLatency->unsigned32BitValue();
	else
		MaxLatency = 0;
	
	Throttler = new AutoThrottler;
	if (Throttler) {
		dbg("Throttler instantiated.\n");
		OSNumber* targetload = (OSNumber*) dict->getObject("TargetCPULoad");
		if (targetload != 0)
			Throttler->targetCPULoad = (targetload->unsigned16BitValue()) * 10;
		else
			Throttler->targetCPULoad = 300;
	}
	
	totalThrottles = 0;
	frequencyUsage[0] = '\0';
	
	/* Return whatever the superclass returned */
	return res;
}

void com_reidburke_air_IntelEnhancedSpeedStep::free(void) {
	dbg("Freeing driver resources\n");
	/* Deallocate the previously allocated spinlock */
	IOSimpleLockFree(Lock);
	super::free();
}

IOService* com_reidburke_air_IntelEnhancedSpeedStep::probe(IOService* provider, SInt32* score) {
	IOService* res = super::probe(provider, score);
	dbg("Probing for Intel processor...\n");
	
	/* Make preliminary check */
	if ( (strcmp(cpuid_info()->cpuid_vendor, CPUID_VID_INTEL) == 0) // Check it's actually Intel
	&& ( cpuid_info()->cpuid_features & CPUID_FEATURE_EST) ) { // Check it supports EST
		*score += 1000;
		dbg("Supported Intel processor found on your system\n");
		res = this;
	} else {
		warn("No Intel processor found, or your processor does not support SpeedStep."
		     "Kext will not load\n");
		res = NULL;
	}
	
	if (!isConstantTSC()) {
		ConstantTSC = false;
		if (RtcFixKernel)
			dbg("Your processor doesn't support constant_tsc, but you have a kernel which can compensate for it.\n");
		else
			warn("Your processor doesn't support constant_tsc and your kernel doesn't " 
			     "know how to compensate - Expect timing issues or switch to a kernel with RTC fix.\n");
	} else {
		ConstantTSC = true;
		Below1Ghz = true; // blindly, because we're not gonna recalibrate the clock so no fear of panic
	}
	
	return res;
}

bool com_reidburke_air_IntelEnhancedSpeedStep::start(IOService *provider) {
	bool res = super::start(provider);
	if (!res) return false;
	
	dbg("Starting\n");
	
	/* Create PState tables */
	if (!createPStateTable(PStates, &NumberOfPStates))
		return false;
	
	// Set the frequency list for sysctl
	char* freqList = getFreqList();
	strcpy(frequencyList, freqList);
	delete[] freqList;
	
	char* voltageList = getVoltageList(true);
	strcpy(originalVoltages, voltageList);
	delete[] voltageList;
	
	
	sysctl_register_oid(&sysctl__kern_cputhrottle_curfreq); 
	sysctl_register_oid(&sysctl__kern_cputhrottle_curvolt);
	sysctl_register_oid(&sysctl__kern_cputhrottle_freqs);
	sysctl_register_oid(&sysctl__kern_cputhrottle_usage);
	sysctl_register_oid(&sysctl__kern_cputhrottle_avgfreq);
	sysctl_register_oid(&sysctl__kern_cputhrottle_factoryvolts);
	sysctl_register_oid(&sysctl__kern_cputhrottle_totalthrottles);
	sysctl_register_oid(&sysctl__kern_cputhrottle_ctl);
	
	if (DefaultPState != -1 && DefaultPState < NumberOfPStates) // If a default Pstate was specified in info.plist
	{
		dbg("Throttling to default PState %d as specified in Info.plist\n", DefaultPState);
		throttleAllCPUs(&PStates[DefaultPState]); // then throttle to that value
	}
	
	// Now turn on our auto-throttler
	if (Throttler->setup((OSObject*) Throttler) == false)
		warn("Auto-throttler could not be setup, start it manually later.\n");
	else
		Throttler->setEnabled(true);
	
	return true;
}

void com_reidburke_air_IntelEnhancedSpeedStep::stop(IOService *provider) {
	dbg("Shutting down\n");
	if (Throttler) {
		Throttler->destruct();
		Throttler->release();
		Throttler = 0;
	}
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_curfreq); 
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_freqs);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_curvolt);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_avgfreq);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_factoryvolts);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_ctl);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_usage);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_totalthrottles);
	
	super::stop(provider);
}

bool isConstantTSC() {
	/* Check for constant_tsc by getting family, model, stepping */
	/* Ref http://en.wikipedia.org/wiki/CPUID#EAX.3D1:_Processor_Info_and_Feature_Bits 
	 * And http://www.intel.com/assets/pdf/appnote/241618.pdf
	 */
	uint8_t cpumodel = (cpuid_info()->cpuid_extmodel << 4) + cpuid_info()->cpuid_model;
	uint8_t cpufamily = cpuid_info()->cpuid_family;
	dbg("Processor Family %d, Model %d\n", cpufamily, cpumodel);
	if ((cpufamily == 0x6 && cpumodel < 14) // 13 is pentium M, 14+ is core and above
	|| ( cpufamily == 0xf && cpumodel < 3)) // 0xF is pentium 4, less than model 3 dont support constant tsc
		// Ref - http://www.tomshardware.com/forum/128629-28-intel
		return false;
	else
		return true;
}

void checkForPenryn() {
	uint8_t cpumodel = (cpuid_info()->cpuid_extmodel << 4) + cpuid_info()->cpuid_model;
	Is45nmPenryn = (cpuid_info()->cpuid_family == 6) && (cpumodel >= 0x17);
	if (Is45nmPenryn)
		dbg("On your processor, voltages can be changed in 12.5 mV steps\n");
	else
		dbg("On your processor, voltages can be changed in 16 mV steps\n");
}

void checkForNby2Ratio() {
	uint64_t sts;
	sts = rdmsr64(INTEL_MSR_PERF_STS);
	Nby2Ratio = (sts & (1ULL << 46)); // bit 46 is set
}

uint16_t getCurrentVoltage() {
	// Apple recommends not to return cached value, but to read it from the processor
	uint64_t msr = rdmsr64(INTEL_MSR_PERF_STS);
	return VID_to_mV(VID(msr));
}

uint16_t getCurrentFrequency() {
	uint64_t msr = rdmsr64(INTEL_MSR_PERF_STS);
	return FID_to_MHz(FID(msr));
}

uint16_t VID_to_mV(uint8_t VID) {
	if (Is45nmPenryn)
		return (((int)VID * 125) + 7125) / 10; // to avoid using float
	else
		return (((int)VID * 16) + 700);
}

uint8_t mV_to_VID(uint16_t mv) {
	if (Is45nmPenryn)
		return ((mv * 10) - 7125) / 125;
	else
		return (mv - 700) / 16;
}

inline uint16_t FID_to_MHz(uint8_t x) {
	bool nby2 = x & 0x80;
	uint8_t realfid = x & 0x7f; // removes the bit from 0x80
	if (nby2)
		realfid /= 2;
	return realfid * (FSB / 1000000ULL);
}

static inline uint32_t FID_to_Hz(uint8_t x) {
	bool nby2 = x & 0x80;
	uint8_t realfid = x & 0x7f;
	if (nby2)
		realfid /= 2;
	return realfid * FSB;
}

inline uint8_t MHz_to_FID(uint16_t x) {
	uint8_t realfid = x / (FSB / 1000000ULL);
	if (Nby2Ratio && x < 1000) // FIXME: use n/2 for only <1ghz frequencies?
		realfid = (realfid*2) | 0x80;
	return realfid;
}

void loadPStateOverride(OSArray* dict) {
	/* Here we load the override pstate table from the given array */
	NumberOfPStates = dict->getCount();
	for (int i = 0; i < NumberOfPStates; i++) {
		OSArray* onePstate		= (OSArray*) dict->getObject(i);
		PStates[i].AcpiFreq		= ((OSNumber*) onePstate->getObject(0))->unsigned16BitValue();
		PStates[i].Frequency		= MHz_to_FID(PStates[i].AcpiFreq); // this accounts for N/2 automatically
		PStates[i].OriginalVoltage	= mV_to_VID(((OSNumber*) onePstate->getObject(1))->unsigned16BitValue());
		PStates[i].Voltage		= PStates[i].OriginalVoltage;
		PStates[i].TimesChosen		= 0;
		dbg("P-State %d: %d MHz at %d mV\n", i, PStates[i].AcpiFreq, VID_to_mV(PStates[i].OriginalVoltage));
	}
	info("Loaded %d PStates from Info.plist\n", NumberOfPStates);
}

bool getFSB() {
	FSB = 0;
	IORegistryEntry* efi = IORegistryEntry::fromPath("/efi/platform", IORegistryEntry::getPlane("IODeviceTree"));
	if (efi == 0) {
		warn("EFI registry entry not found!\n");
		return false;
	}
	OSData* fsb = (OSData*) efi->getProperty("FSBFrequency");
	if (fsb == 0) {
		warn("FSB frequency entry not found!\n");
		return false;
	}
	bcopy(fsb->getBytesNoCopy(), &FSB, 8);
	dbg("FSB = %d MHz\n", FSB/1000000ULL);
	efi = 0; fsb = 0; // in dono ka kaam khatam	
	return true;
}

bool createPStateTable(PState* pS, unsigned int* numStates) {	
	checkForPenryn(); // early on, so we can display proper mV values
	
	/* If the PState table was specified manually, we dont do the rest. Otherwise autodetect */
	if (NumberOfPStates != 0) {
		dbg("PState table was already created. No autodetection will be performed\n");
		return true;
	}
	
	/* Find CPUs in the IODeviceTree plane */
	IORegistryEntry* ioreg = IORegistryEntry::fromPath("/cpus", IORegistryEntry::getPlane("IODeviceTree"));
	if (ioreg == 0) {
		warn("Holy moly we cannot find your CPU!\n");
		return false;
	}
	
	/* Get the first CPU - we assume all CPUs share the same P-State */
	IOACPIPlatformDevice* cpu = (IOACPIPlatformDevice*) ioreg->getChildEntry(IORegistryEntry::getPlane("IODeviceTree"));
	if (cpu == 0) {
		warn("Um you don't seem to have a CPU o.O\n");
		ioreg = 0;
		return false;
	}
	
	dbg("Using data from %s\n", cpu->getName());
	
	/* Now try to find the performance state table */
	OSObject* PSS; uint32_t bootarg;
	cpu->evaluateObject("_PSS", &PSS);
	if(PSS == 0 || PE_parse_boot_arg("-autopstates", &bootarg)) {
		warn("Auto-creating a PState table.\n");
		int maxFID = MHz_to_FID(getCurrentFrequency());
		int maxVID = mV_to_VID(getCurrentVoltage());
		int minVID = mV_to_VID(984); // For now we'll use hardcoded minvolt, later use table
		int minFID = 6; // No LFM right now
		NumberOfPStates = 1 + ((maxFID - minFID) / 2);
		for (int i = 1; i < NumberOfPStates; i++) {
			PStates[i].Frequency		= minFID + (2*(NumberOfPStates - i - 1));
			PStates[i].AcpiFreq		= FID_to_MHz(PStates[i].Frequency);
			PStates[i].OriginalVoltage	= maxVID - (i*((maxVID - minVID) / NumberOfPStates));
			PStates[i].Voltage		= PStates[i].OriginalVoltage;
			PStates[i].Latency		= 110;
			PStates[i].TimesChosen		= 0;
		}
		
		PStates[0].Frequency		= maxFID;
		PStates[0].AcpiFreq		= FID_to_MHz(maxFID);
		PStates[0].OriginalVoltage	= maxVID;
		PStates[0].Voltage		= PStates[0].OriginalVoltage;
		PStates[0].Latency		= 110;
		PStates[0].TimesChosen		= 0;
		MaxLatency			= PStates[0].Latency;
		info("Using %d PStates (auto-created, may not be optimal).\n", NumberOfPStates);
		ioreg = 0; cpu = 0;
		return true;
	}
	
	OSArray* PSSArray = (OSArray*) PSS;
	NumberOfPStates = PSSArray->getCount();
	dbg("Found %d P-States\n", NumberOfPStates);
	OSArray* onestate; uint16_t ctl, acpifreq; uint32_t power, latency;
	int i = 0, c = 0;
	
	while (c < PSSArray->getCount()) {
		onestate = ( OSArray* )(PSSArray->getObject(c));
		ctl      = ((OSNumber*) onestate->getObject(4))->unsigned32BitValue();
		acpifreq = ((OSNumber*) onestate->getObject(0))->unsigned32BitValue();
		power	 = ((OSNumber*) onestate->getObject(1))->unsigned32BitValue();
		latency	 = ((OSNumber*) onestate->getObject(2))->unsigned32BitValue();
		c++;
		
		if (acpifreq - (10 * (acpifreq / 10)) == 1) {
			// most likely spurious, so skip it
			warn("** Spurious P-State %d: %d MHz at %d mV, consuming %d W, latency %d usec\n", i, acpifreq, VID_to_mV(ctl), power / 1000, latency);
			NumberOfPStates--;
			continue;
		}
		
		if (acpifreq < 1000 && !Below1Ghz) {
			warn("%d MHz disabled because your processor or kernel doesn't support it.\n");
			NumberOfPStates--;
			continue;
		}
		
		PStates[i].AcpiFreq		= acpifreq; // cosmetic only
		PStates[i].Frequency		= FID(ctl);
		PStates[i].OriginalVoltage	= VID(ctl);
		PStates[i].Voltage		= PStates[i].OriginalVoltage; // initially same
		PStates[i].Latency		= latency;
		PStates[i].TimesChosen		= 0;
		
		if (latency > MaxLatency) MaxLatency = latency;
		
		dbg("P-State %d: %d MHz at %d mV, consuming %d W, latency %d usec\n",
		    i, PStates[i].AcpiFreq, VID_to_mV(PStates[i].OriginalVoltage),
		    power / 1000, latency);
		i++;
	}
	
	info("Using %d PStates.\n", NumberOfPStates);
	
	ioreg = 0; cpu = 0; PSS = 0; onestate = 0;
	return true;
}

/**************************************************************************************************/
/* Throttling functions */

void throttleAllCPUs(PState* p) {
	dbg("Starting throttle with CTL 0x%x\n", CTL(p->Frequency, p->Voltage));
	IOSimpleLockLock(Lock);
	
	mp_rendezvous(disableInterrupts, throttleCPU, enableInterrupts, p);
	IODelay(p->Latency); // maybe wait longer?
	
	totalThrottles++;
	
	IOSimpleLockUnlock(Lock);
	dbg("Throttle done.\n");
}

void throttleCPU(void *t) {
	uint64_t msr;
	PState p;
	uint32_t newfreq, oldfreq;

	bcopy(t,&p,sizeof(PState)); // get the ctl we want
	
	msr = rdmsr64(INTEL_MSR_PERF_STS); // read current MSR
	
	// For clock recalibration
	oldfreq = FID_to_Hz(FID(msr));
	// blank out last 32 bits and put our ctl there
	msr = (msr & 0xffffffffffff0000ULL) | CTL(p.Frequency, mV_to_VID(1000));
	newfreq = FID_to_Hz(FID(msr)); // after setting ctl in msr
	
	if (RtcFixKernel && !ConstantTSC) {
		rtc_clock_stepping(newfreq, oldfreq);
	}
	
	wrmsr64(INTEL_MSR_PERF_CTL, msr); // and write it to the processor
	
	if (RtcFixKernel && !ConstantTSC)
		rtc_clock_stepped(newfreq, oldfreq);
}

void disableInterrupts(__unused void *t) {
	InterruptsEnabled = ml_set_interrupts_enabled(false);
}

void enableInterrupts(__unused void *t) {
	ml_set_interrupts_enabled(InterruptsEnabled);
}




/**********************************************************************************************************/


static int iess_handle_targetload SYSCTL_HANDLER_ARGS
{
	int err = 0;
	if (req->newptr) {
		// new ctl being set
		int target;
		err = SYSCTL_IN(req, &target, sizeof(int));
		if (err) return err;
		if (!Throttler) return kIOReturnError;
		if (target > 95) return kIOReturnError;
		dbg("Setting autothrottle target to %d\n", target*10);
		Throttler->targetCPULoad = target * 10;
		
	} else {
		if (!Throttler) return kIOReturnError;
		int target = Throttler->targetCPULoad / 10;
		err = SYSCTL_OUT(req, &target, sizeof(int));
	}
	return err;
}

SYSCTL_PROC  (_kern, OID_AUTO, cputhrottle_targetload,	CTLTYPE_INT | CTLFLAG_RW, 0, 0, &iess_handle_targetload,    "I", "Auto-throttle target CPU load");

bool AutoThrottler::setup(OSObject* owner) {
	if (setupDone) return true;
	
	workLoop = IOWorkLoop::workLoop();
	if (workLoop == 0) return false;
	
	perfTimer = IOTimerEventSource::timerEventSource(owner, (IOTimerEventSource::Action) &perfTimerWrapper);
	if (perfTimer == 0) return false;
	
	/* from Superhai (modified by mercurysquad) */
	cpu_count = 0; OSDictionary* service;
	mach_timespec_t serviceTimeout = { 60, 0 }; // in seconds
	totalTimerEvents = 0;
	
	IOService* firstCPU = IOService::waitForService(IOService::serviceMatching("IOCPU"), &serviceTimeout);

	if (!firstCPU) {
		warn("IOKit CPUs not found. Auto-throttle may not work.\n");
		return false;
	} else {
		// we got first cpu, so the others should also be available by now. get them
		service = IOService::serviceMatching("IOCPU");
	}
	
	OSIterator* iterator = IOService::getMatchingServices(service);
	
	if (!iterator) {
		warn("IOKit CPU iterator couldn't be created. Auto-throttle may not work.\n");
		return false;
	}

	IOCPU * cpu;
	while (cpu = OSDynamicCast(IOCPU, iterator->getNextObject()))
	{
		dbg("Got I/O Kit CPU %d (%d) named %s", cpu_count, cpu->getCPUNumber(), cpu->getCPUName()->getCStringNoCopy());
		mach_cpu[cpu_count] = cpu->getMachProcessor();
		if (cpu_count++ > max_cpus) break;
	}
	selfHost = host_priv_self();
	if (workLoop->addEventSource(perfTimer) != kIOReturnSuccess) return false;
	currentPState = NumberOfPStates - 1;
	perfTimer->setTimeoutMS(throttleQuantum * (1 + currentPState));
	clock_get_uptime(&lastTime);
	if (!targetCPULoad) targetCPULoad = defaultTargetLoad; // % x10
	sysctl_register_oid(&sysctl__kern_cputhrottle_targetload);
	sysctl_register_oid(&sysctl__kern_cputhrottle_auto);
	setupDone = true;
	return true;
}


void AutoThrottler::stop() {
	enabled = false;
	perfTimer->cancelTimeout();
	perfTimer->disable();
	// Settle time in case we were just stepping
	IOSleep(1024);
	if (workLoop) workLoop->removeEventSource(perfTimer);	// Remove our event sources
	dbg("Autothrottler stopped.\n");
	setupDone = false;
}

void AutoThrottler::setEnabled(bool _enabled) {
	if (_enabled) {
		// asked to turn on
		perfTimer->enable();
	} else {
		perfTimer->cancelTimeout();
		perfTimer->disable();
	}
	enabled = _enabled;
}

bool AutoThrottler::getEnabled() {
	return enabled;
}

void AutoThrottler::destruct() {
	if (enabled) enabled = false;
	if (setupDone) stop();
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_targetload);
	sysctl_unregister_oid(&sysctl__kern_cputhrottle_auto);
	if (perfTimer) {
		perfTimer->release();
		perfTimer = 0;
	}
	
	if (workLoop) {
		workLoop->release();
		workLoop = 0;
	}
}


void AutoThrottler::GetCPUTicks(long* idle, long* total) {
	mach_msg_type_number_t cpu_count_type;
	unsigned int cpu_maxload = 0;
	/* Superhai method */
	unsigned int temp_ticks = 0;
	for (int i = 0; i < cpu_count; i++)
	{
		cpu_count_type = PROCESSOR_CPU_LOAD_INFO_COUNT;
		total_ticks[i] = 0;
		kern_return_t kret = processor_info(mach_cpu[i], PROCESSOR_CPU_LOAD_INFO, &selfHost,
						    (processor_info_t) &cpu_load[i], &cpu_count_type);
		if (kret != KERN_SUCCESS)
		{
			dbg("Error when reading cpu load on cpu %d (%x)", i, kret);
			break;
		}
		cpu_load_temp[i] = cpu_load[i];
		for (int t = 0; t < CPU_STATE_MAX; t++)
		{
			(cpu_load_temp[i].cpu_ticks[t]) -= (cpu_load_last[i].cpu_ticks[t]);
			total_ticks[i] += (cpu_load_temp[i].cpu_ticks[t]);
		}
		load_ticks[i] = total_ticks[i] - cpu_load_temp[i].cpu_ticks[CPU_STATE_IDLE];
		if ((load_ticks[i]) > temp_ticks)
		{
			temp_ticks = load_ticks[i];
			cpu_maxload = i;
		}
		cpu_load_last[i] = cpu_load[i];
	}
	
	*total = total_ticks[cpu_maxload];
	*idle  = *total - load_ticks[cpu_maxload];
	
	dbg("Autothrottle: CPU load %d /10 pc\n", (1000 * (*total - *idle)) / *total);
}



bool perfTimerWrapper(OSObject* owner, IOTimerEventSource* src, int count) {
	register AutoThrottler* objDriver = (AutoThrottler*) owner;
	return (objDriver->perfTimerEvent(src, count));
}


bool AutoThrottler::perfTimerEvent(IOTimerEventSource* src, int count) {
	uint32_t wantspeed,wantstep;
	long idle, used, total;
	uint64_t msr;
	
	msr = rdmsr64(INTEL_MSR_PERF_STS); // read current MSR
	// For clock recalibration
	

	
	
	if (!enabled || !setupDone) return false;
	
	// gather stats
	PStates[currentPState].TimesChosen++;
	totalTimerEvents++;
	
	GetCPUTicks(&idle, &total);
	
	// Used = % used x 10
	used = ((total - idle) * 1000) / total;
	
	// If used > 95% we can't really guess how much is needed, so step to highest speed

	if (FID(msr) ==! PStates[0].Frequency);
	{
		wantspeed = PStates[0].AcpiFreq;
	}
	wantstep = FindClosestPState(wantspeed);
	if (VID(msr) == 975) goto check_soon;	
	currentPState = wantstep; // Assume we got the one we wanted
	if(VID(msr) ==! mV_to_VID(975));
	{
		PStates[currentPState].Voltage = mV_to_VID(975);
	}
	
//
		// blank out last 32 bits and put our ctl there

//		
	
	throttleAllCPUs(&PStates[currentPState]);
	
	perfTimer->setTimeoutMS(throttleQuantum * (NumberOfPStates - wantstep)); // Make the delay until the next check proportional to the speed we picked
	return true;
	
check_soon:
	perfTimer->setTimeoutMS(throttleQuantum);
	return true;
}


