#define kextname	"TurboEIST"
#define driver		TurboEIST
#define super		IOService

#include "TurboEIST.h"

#define dbg(args...)    do { IOLog(kextname ": DEBUG " args); IOSleep(50); } while(0)
#define err(args...)    do { IOLog(kextname ": ERROR " args); IOSleep(50); } while(0)
#define info(args...)   do { IOLog(kextname ": INFO " args); IOSleep(50); } while(0)

#define intdbg(args...) IOLog(kextname ": INTDBG " args);
#define chkpoint(arg)   printf(kextname " %s : %s\n", __FUNCTION__, arg)
#define fail(arg)  do { printf(kextname ": %s(%d) - fail '%s'\n", __FUNCTION__, __LINE__, arg); goto fail; } while(0)

#define RELEASE(x) do { if(x) { (x)->release(); (x) = 0; } } while(0)

OSDefineMetaClassAndStructors( driver, IOService )	;

// Operations for building valid VIDs from MHZ/MV settings
/*
#define VID_TO_MV(vid)  ((vid & 0xff) * 16 + 700)
#define VID_TO_MHZ(vid) ((vid>>8) * 100)
#define MHZ_TO_VID(mhz) ((mhz/100) << 8)
#define MV_TO_VID(mv)   (((mv) - 700) / 16)
*/
// NEW penyrn/core2duo!
#define VID_TO_MV(vid)  ((vid & 0xff) * 12.5 + 712.5)
#define VID_TO_MHZ(vid) ((vid>>8) * 200)
#define MHZ_TO_VID(mhz) ((mhz/200) << 8)
#define MV_TO_VID(mv)   (((mv) - 712) / 12)

#define VID(mhz,mv)	( MHZ_TO_VID(mhz) | MV_TO_VID(mv) )

#define SLEEPYTIME              500     // Thousands of a second
#define TARGET_CPULOAD          200     // Target this idle percentage (x10)
#define MHZ_IGNORE_THRESHOLD    201     // Ignore speed changes below this threshold



typedef struct {
 uint16_t cpuMhz;
 uint16_t cpuMv;
 uint16_t VID;
} speedstep_cpu_t;

speedstep_cpu_t speedstep_cpu_setting[15];
uint16_t num_speeds = 0;

#define CTL_VM          2               /* virtual memory */
#define VM_LOADAVG      2               /* struct loadavg */

#define kVID "mhz,mv"

// finding the header is too much of a pain in the ass
struct loadavg {
 fixpt_t ldavg[3];
};

// Our performance info
extern volatile  host_cpu_load_info_data_t avenrun;


// forward
bool perfTimerWrapper(OSObject *owner, IOTimerEventSource *src, int count);


static int get_cpu_ticks(long * idle, long * total) {
    host_cpu_load_info_data_t loadinfo;
	static long idle_old, total_old;
	long total_new;
	mach_msg_type_number_t count;

	count = HOST_CPU_LOAD_INFO_COUNT; 
	host_statistics(host_priv_self(),HOST_CPU_LOAD_INFO,(host_info_t)&loadinfo,&count);

	total_new = loadinfo.cpu_ticks[CPU_STATE_USER]+loadinfo.cpu_ticks[CPU_STATE_NICE]+loadinfo.cpu_ticks[CPU_STATE_SYSTEM]+loadinfo.cpu_ticks[CPU_STATE_IDLE];

	if (idle)
		*idle =	loadinfo.cpu_ticks[CPU_STATE_IDLE] - idle_old;
	if (total)
		*total = total_new - total_old;

	idle_old = loadinfo.cpu_ticks[CPU_STATE_IDLE];
	total_old = total_new;

	return 0;
}


static void eist_update_action(void *t) {
 uint64_t msr;
 uint16_t vid;
 uint64_t v64;

 bcopy(t,&vid,2);

 v64=vid;
 
 msr = rdmsr64(MSR_PERF_CTL);
 msr = (msr & ~(uint64_t)(0xffff)) | v64;
 wrmsr64(MSR_PERF_CTL, msr);

 //intdbg("CPU%d: %s vid=%d\n", get_cpu_number(), __FUNCTION__,vid);
}

static void eist_update_setup(__unused void * param_not_used) {
	/* disable interrupts before the first barrier */
	current_cpu_datap()->cpu_iflag = ml_set_interrupts_enabled(false);
}

static void eist_update_teardown(__unused void * param_not_used) {
	/* restore interrupt flag following changes */
	ml_set_interrupts_enabled(current_cpu_datap()->cpu_iflag);
}

/*
 * Update EIST settings on all processors.
 */
void driver::eist_update_all_cpus(uint16_t vid) {
 void *p;
        
 IOSimpleLockLock(eist_lock);
 p=&vid;
 mp_rendezvous(eist_update_setup, eist_update_action, eist_update_teardown, p);
 IOSimpleLockUnlock(eist_lock);
}


bool driver::init(OSDictionary *properties) {
    OSArray *speedArray = NULL;
    OSCollectionIterator *speedIterator = NULL;
    OSString *speedKey;

    chkpoint("Initializing\n");
    if (super::init(properties) == false) fail("super::init");
	
    speedArray = OSDynamicCast(OSArray, getProperty(kVID));
    if (speedArray == NULL) fail("no " kVID "!\n");

    speedIterator = OSCollectionIterator::withCollection(speedArray);
    if (speedIterator == NULL) fail("no speediterator!\n");

    speedIterator->reset();
    while (speedKey = (OSString *)speedIterator->getNextObject()) {
         sscanf(speedKey->getCStringNoCopy(),"%d,%d,%x",&speedstep_cpu_setting[num_speeds].cpuMhz,&speedstep_cpu_setting[num_speeds].cpuMv,&speedstep_cpu_setting[num_speeds].VID);
	 if (speedstep_cpu_setting[num_speeds].cpuMhz<=100 || speedstep_cpu_setting[num_speeds].cpuMv<=700 || speedstep_cpu_setting[num_speeds].VID < 0xFF) { // Some sane values - 700mv is the absolute minimum possible?
	  bzero(&speedstep_cpu_setting[num_speeds],sizeof(speedstep_cpu_t));
	  err("Ignoring invalid mhz,mv specified = %s\n",speedKey->getCStringNoCopy());
	 } else {
	  //speedstep_cpu_setting[num_speeds].VID=VID(speedstep_cpu_setting[num_speeds].cpuMhz,speedstep_cpu_setting[num_speeds].cpuMv);
	  info("Identified configured speed %uMhz, %umv, VID=0x%04x\n",speedstep_cpu_setting[num_speeds].cpuMhz,speedstep_cpu_setting[num_speeds].cpuMv,speedstep_cpu_setting[num_speeds].VID);
          num_speeds++;
	 }
    }
	
    if (!num_speeds) fail("No valid speeds given"); // No valid settings, bail!
    dbg("%d speed settings used\n", num_speeds);

    eist_lock=IOSimpleLockAlloc();
    if (!eist_lock) fail("IOSimpleLockAlloc");
	
    return(true);
	
    fail:
    return(false);
}

bool driver::start(IOService *provider) {
 uint32_t rc;
   
 chkpoint("begin");
 if (super::start(provider)==false) return(false);	// Start our superclass first

 // Get our workloop?
 createWorkLoop();

 if (!workLoop) fail("workLoop");
   

 // Set up a timer event source for performance monitoring
 perfTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action)&perfTimerWrapper);
 if (!perfTimer) fail("perfTimer");
 if (workLoop->addEventSource(perfTimer) != kIOReturnSuccess) fail("workLoop->addEventSource(perfTimer)");
 
 // Set driver status
 driverStatus = driverPark;

 stepCurrent = num_speeds - 1;
 
 driverStatus = driverDrive;
 
 perfTimer->setTimeoutMS(1024);
 clock_get_uptime(&lastTimer);

 chkpoint("success");
 return(true);
 
 fail:
 stop(provider);
 return(false);
}

void driver::stop(IOService *provider) {
 // We have stopped
 driverStatus = driverOff;
 perfTimer->cancelTimeout();

 // Settle time in case we were just stepping
 IOSleep(1024);
 
 if (workLoop) workLoop->removeEventSource(perfTimer);	// Remove our event sources
 super::stop(provider);
}

void driver::free(void) {
 chkpoint("begin");

 RELEASE(perfTimer);
 RELEASE(workLoop);

 IOSimpleLockFree(eist_lock);
 
 super::free();
 chkpoint("end");
}	


bool driver::createWorkLoop(void) {
 workLoop = IOWorkLoop::workLoop();
 return (workLoop != 0);
}

IOWorkLoop *driver::getWorkLoop(void) const {
 return(workLoop);
}


bool perfTimerWrapper(OSObject *owner, IOTimerEventSource *src, int count) {
 register driver *objDriver = (driver *) owner;
 return(objDriver->perfTimerEvent(src,count));
}

// Real timer filter
bool driver::perfTimerEvent(IOTimerEventSource *src, int count) {
 uint64_t msr;
 uint32_t vid;
 
 uint32_t next_check_delay;
 uint32_t wantspeed,wantstep;
 long i,idle,used,total;

 if (driverStatus != driverDrive) return(false);

 //ok, so we're using adaptive mode..
 if (get_cpu_ticks(&idle, &total)) fail("get_cpu_load");

 // Used = % used x 10
 used = ((total-idle)*1000)/total;

 // If used > 95% we can't really guess how much is needed, so step to highest speed
 if (used>=950) wantspeed=speedstep_cpu_setting[num_speeds-1].cpuMhz;
		
 // Otherwise wantspeed is the ideal frequency to maintain idle % target
 else wantspeed = (speedstep_cpu_setting[stepCurrent].cpuMhz * (used+1)) / TARGET_CPULOAD;

 if (wantspeed>=speedstep_cpu_setting[num_speeds-1].cpuMhz)	wantstep = num_speeds-1;	// We want something higher than our highest
 else if (wantspeed<=speedstep_cpu_setting[0].cpuMhz)			wantstep = 0;				// We want something lower than our lowest
 
 // Otherwise, find the best step
 else for (i = 0; i < (num_speeds-1); i++) if ((wantspeed >= speedstep_cpu_setting[i].cpuMhz) && (wantspeed < speedstep_cpu_setting[i+1].cpuMhz)) wantstep=i;
 //dbg("pctused=%u idle=%u total=%u wantspeed=%u wantstep=%u\n",used,idle,total,wantspeed,wantstep);

 if (wantstep == stepCurrent) goto check_soon;

 stepCurrent = wantstep; // Assume we got the one we wanted

 eist_update_all_cpus(speedstep_cpu_setting[wantstep].VID);
// IOSleep(1);
 msr = rdmsr64(MSR_PERF_CTL);
 vid = msr & 0xFFFF;
 dbg("Stepping to %u pctused=%u VID=0x%04x\n",speedstep_cpu_setting[wantstep].cpuMhz,used,vid);
 
 perfTimer->setTimeoutMS(512 * (wantstep+1)); // Make the delay until the next check proportional to the speed we picked
 return(true);
 

 perfTimer->setTimeoutMS(1024);
 success:
 return(true);

 check_soon:
 perfTimer->setTimeoutMS(512);
 return(true);

 fail:
 return(false);
}

// Callback for registering how we will handle power state changes
// These specific values return information that means we want to use disable/enable for handling power state transitions
IOReturn driver::registerWithPolicyMaker(IOService *policyMaker) {
 IOReturn ior;

 enum {
  kPowerStateOff = 0,
  kPowerStateOn,
  kPowerStateCount
 };

 static IOPMPowerState powerStateArray[ kPowerStateCount ] = {
   { 1,0,0,0,0,0,0,0,0,0,0,0 },
   { 1,kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0 }
 };

 chkpoint("entered");
 //fCurrentPowerState = kPowerStateOn;
 return(policyMaker->registerPowerDriver(this, powerStateArray, kPowerStateCount));
}
