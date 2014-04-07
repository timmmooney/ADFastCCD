/**
 * Area Detector driver for the LBNL FastCCD
 * Modifed by S. Wilkins
 *
 */

#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsString.h>
#include <iocsh.h>
#include <epicsExport.h>
#include <epicsExit.h>

#include "cin.h"
#include "FastCCD.h"

static const char *driverName = "FastCCD";

//Definitions of static class data members

//const epicsUInt32 FastCCD::ATInternal = 0;
//const epicsUInt32 FastCCD::ATExternal1 = 1;
//const epicsUInt32 FastCCD::ATExternal2 = 2;
//const epicsUInt32 FastCCD::ATExternal1or2 = 3;

asynStatus FastCCD::connect(asynUser *pasynUser){
  return connectCamera();
}

asynStatus FastCCD::connectCamera(){

  if(cin_data_init_port(&cin_data_port, NULL, 0, "10.23.5.127", 0, 1000))
  {
    return asynError;
  }
  if(cin_ctl_init_port(&cin_ctl_port, NULL, 0, 0))
  {
    return asynError;
  }
  if(cin_ctl_init_port(&cin_ctl_port_stream, NULL, 49202, 50202))
  {
    return asynError;
  }
  if(cin_data_init(CIN_DATA_MODE_CALLBACK, cinPacketBuffer, cinImageBuffer,
                   allocateImageC, processImageC, this))
  {
    return asynError;
  }

  return asynSuccess;
}

asynStatus FastCCD::disconnect(asynUser *pasynUser){
  return disconnectCamera();
}

asynStatus FastCCD::disconnectCamera(){
  cin_ctl_close_port(&cin_ctl_port);
  cin_ctl_close_port(&cin_ctl_port_stream);
  return asynSuccess; 
}

static void allocateImageC(cin_data_frame_t *frame){
  FastCCD *ptr = (FastCCD*)frame->usr_ptr;
  ptr->allocateImage(frame);
}

void FastCCD::allocateImage(cin_data_frame_t *frame)
{
  size_t dims[2];
  int nDims = 2;
   
  dims[0] = CIN_DATA_FRAME_WIDTH;
  dims[1] = CIN_DATA_FRAME_HEIGHT;
  NDDataType_t dataType = NDUInt16; 
  
  while(!(pImage = this->pNDArrayPool->alloc(nDims, dims, dataType, 
                                        0, NULL)))
  {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "Unable to allocate array from pool....\n");
    sleep(1);
  }
      
  frame->data = (uint16_t*)pImage->pData;

  return;
}

static void processImageC(cin_data_frame_t *frame)
{
  FastCCD *ptr = (FastCCD*)frame->usr_ptr;
  ptr->processImage(frame);
}

void FastCCD::processImage(cin_data_frame_t *frame)
{
  const char* functionName = "processImage";
  this->lock();

  // Set the unique ID
  pImage->uniqueId = frame->number;

  // TODO : Do timestamp processing using cin_data_frame
  
  // Get any attributes for the driver
  this->getAttributes(pImage->pAttributeList);
       
  int arrayCallbacks;
  getIntegerParam(NDArrayCallbacks, &arrayCallbacks);

  if (arrayCallbacks) {
    /* Call the NDArray callback */
    /* Must release the lock here, or we can get into a deadlock, because we can
     * block on the plugin lock, and the plugin can be calling us */
    this->unlock();
    doCallbacksGenericPointer(pImage, NDArrayData, 0);
    this->lock();
  }

  if (this->framesRemaining > 0) this->framesRemaining--;
  if (this->framesRemaining == 0) {
    setIntegerParam(ADAcquire, 0);
    setIntegerParam(ADStatus, ADStatusIdle);
  }

  /* Update the frame counter */
  int imageCounter;
  getIntegerParam(NDArrayCounter, &imageCounter);
  imageCounter++;
  setIntegerParam(NDArrayCounter, imageCounter);
 
  asynPrint(this->pasynUserSelf, ASYN_TRACEIO_DRIVER,
              "%s:%s: frameId=%d\n",
              driverName, functionName, frame->number);

  // Release the frame as we are done with it!
  // SBW Not sure we should do this!!!!!!
  pImage->release();

  /* Update any changed parameters */
  callParamCallbacks();

  this->unlock();
  return;
}

/** Constructor for FastCCD driver; most parameters are simply passed to ADDriver::ADDriver.
  * After calling the base class constructor this method creates a thread to collect the detector data, 
  * and sets reasonable default values the parameters defined in this class, asynNDArrayDriver, and ADDriver.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  */
FastCCD::FastCCD(const char *portName, int maxBuffers, size_t maxMemory, 
                 int priority, int stackSize, int packetBuffer, int imageBuffer)

  : ADDriver(portName, 1, NUM_FastCCD_DET_PARAMS, maxBuffers, maxMemory, 
             asynEnumMask, asynEnumMask,
             ASYN_CANBLOCK, 1, priority, stackSize)
{

  int status = asynSuccess;
  int sizeX, sizeY;
  
  static const char *functionName = "FastCCD";

  /* Write the packet and frame buffer sizes */
  cinPacketBuffer = packetBuffer;
  cinImageBuffer = imageBuffer;

  //Define the polling periods for the status thread.
  mPollingPeriod = 30.0; //seconds
  
  /* Create an EPICS exit handler */
  epicsAtExit(exitHandler, this);

  //createParam(FastCCDSetBiasString,                  asynParamInt32, &FastCCDSetBias);
  //createParam(FastCCDSetClocksString,                asynParamInt32, &FastCCDSetClocks);

  createParam(FastCCDMux1String,                asynParamInt32, &FastCCDMux1);
  createParam(FastCCDMux2String,                asynParamInt32, &FastCCDMux2);

  // Create the epicsEvent for signaling to the status task when parameters should have changed.
  // This will cause it to do a poll immediately, rather than wait for the poll time period.
  this->statusEvent = epicsEventMustCreate(epicsEventEmpty);
  if (!this->statusEvent) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: Failed to create event for status task.\n",
              driverName, functionName);
    return;
  }

  try {
    this->lock();
    connectCamera();
    this->unlock();
    setStringParam(ADStatusMessage, "Initialized");
    callParamCallbacks();
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
    return;
  }

  sizeX = CIN_DATA_FRAME_WIDTH;
  sizeY = CIN_DATA_FRAME_HEIGHT;

  /* Set some default values for parameters */
  status =  setStringParam(ADManufacturer, "Berkeley Laboratory");
  status |= setStringParam(ADModel, "1k x 2k FastCCD"); // Model ?
  status |= setIntegerParam(ADSizeX, sizeX);
  status |= setIntegerParam(ADSizeY, sizeY);
  status |= setIntegerParam(ADBinX, 1);
  status |= setIntegerParam(ADBinY, 1);
  status |= setIntegerParam(ADMinX, 0);
  status |= setIntegerParam(ADMinY, 0);
  status |= setIntegerParam(ADMaxSizeX, sizeX);
  status |= setIntegerParam(ADMaxSizeY, sizeY);  
  status |= setIntegerParam(ADImageMode, ADImageSingle);
  status |= setIntegerParam(ADTriggerMode, 1);
  status |= setDoubleParam (ADAcquireTime, 0.005);
  status |= setDoubleParam (ADAcquirePeriod, 1.0);
  status |= setIntegerParam(ADNumImages, 1);
  status |= setIntegerParam(ADNumExposures, 1);
  status |= setIntegerParam(NDArraySizeX, sizeX);
  status |= setIntegerParam(NDArraySizeY, sizeY);
  status |= setIntegerParam(NDDataType, NDUInt16);
  status |= setIntegerParam(NDArraySize, sizeX*sizeY*sizeof(epicsUInt16)); 
  status |= setDoubleParam(ADShutterOpenDelay, 0.);
  status |= setDoubleParam(ADShutterCloseDelay, 0.);

  callParamCallbacks();

  // Signal the status thread to poll the detector
  epicsEventSignal(statusEvent);
  
  if (stackSize == 0) stackSize = epicsThreadGetStackSize(epicsThreadStackMedium);

  /* Create the thread that updates the detector status */
  //status = (epicsThreadCreate("FastCCDStatusTask",
  //                            epicsThreadPriorityMedium,
  //                            stackSize,
  //                            (EPICSTHREADFUNC)FastCCDStatusTaskC,
  //                            this) == NULL);
  //if(status) {
  //  asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
  //    "%s:%s: Failed to create status task.\n",
  //    driverName, functionName);
  //  return;
  //}

}

/**
 * Destructor.  Free resources and closes the FastCCD library
 */
FastCCD::~FastCCD() 
{
  static const char *functionName = "~FastCCD";

  try {
    this->lock();
    cin_data_stop_threads();
    cin_data_wait_for_threads();
    this->unlock();
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
  }
}


/**
 * Exit handler, delete the FastCCD object.
 */

static void exitHandler(void *drvPvt)
{
  FastCCD *pFastCCD = (FastCCD *)drvPvt;
  delete pFastCCD;
}


//asynStatus FastCCD::readEnum(asynUser *pasynUser, char *strings[], int values[], 
//                             int severities[], size_t nElements, size_t *nIn)
//{
//  //int function = pasynUser->reason;
//
//  return ADDriver::readEnum(pasynUser, strings, values, severities,nElements, nIn );
//  
//}
//

/** Report status of the driver.
  * Prints details about the detector in us if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details Controls the level of detail in the report. */
void FastCCD::report(FILE *fp, int details)
{
  int xsize, ysize;
  static const char *functionName = "report";

  fprintf(fp, "FastCCD CCD port = %s\n", this->portName);
  if (details > 0) {
    try {
      getIntegerParam(ADMaxSizeX, &xsize);
      getIntegerParam(ADMaxSizeY, &ysize);
      fprintf(fp, "  X pixels: %d\n", xsize);
      fprintf(fp, "  Y pixels: %d\n", ysize);

    } catch (const std::string &e) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s:%s: %s\n",
        driverName, functionName, e.c_str());
    }
  }
  // Call the base class method
  ADDriver::report(fp, details);
}


/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including ADAcquire, ADBinX, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus FastCCD::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    epicsInt32 oldValue;

    asynStatus status = asynSuccess;
    static const char *functionName = "writeInt32";

    //Set in param lib so the user sees a readback straight away. Save a backup in case of errors.
    getIntegerParam(function, &oldValue);
    status = setIntegerParam(function, value);

    if (function == ADAcquire) 
    {
      if (value) // User clicked 'Start' button
      {
         // Send the hardware a start trigger command
         int n_images, t_mode, i_mode;
         getIntegerParam(ADTriggerMode, &t_mode);
         getIntegerParam(ADNumImages, &n_images);
         getIntegerParam(ADImageMode, &i_mode);
         switch(i_mode) {
           case ADImageSingle:
             this->framesRemaining = 1;
             n_images = 1;
             break;
           case ADImageMultiple:
             this->framesRemaining = n_images;
             break;
           case ADImageContinuous:
             this->framesRemaining = -1;
             n_images = 0;
             break;
         }

         if(t_mode == 0){
           if(!cin_ctl_int_trigger_start(&cin_ctl_port, n_images)){
             setIntegerParam(ADAcquire, 1);
             setIntegerParam(ADStatus, ADStatusAcquire);
           }
         } else {
           if(!cin_ctl_ext_trigger_start(&cin_ctl_port, t_mode)){
             setIntegerParam(ADStatus, ADStatusAcquire);
             setIntegerParam(ADAcquire, 1);
           }
         }
      }
      else // User clicked 'Stop' Button
      {
         // Send the hardware a stop trigger command
         if(!cin_ctl_int_trigger_stop(&cin_ctl_port)){
           setIntegerParam(ADStatus, ADStatusIdle);
           setIntegerParam(ADAcquire, 0);
         }
         if(!cin_ctl_ext_trigger_stop(&cin_ctl_port)){
           setIntegerParam(ADStatus, ADStatusIdle);
           setIntegerParam(ADAcquire, 0);
         }
      }
    }
    else if ((function == ADNumExposures) || (function == ADNumImages)   ||
             (function == ADImageMode))
    {
      status = ADDriver::writeInt32(pasynUser, value);
    }
    else if (function == FastCCDMux1)
    {
      cin_ctl_set_mux(&cin_ctl_port, value);   
    }
    else if (function == FastCCDMux2)
    {
      cin_ctl_set_mux(&cin_ctl_port, value << 8);
    }

    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();

    if (status)
    {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: error, status=%d function=%d, value=%d\n",
              driverName, functionName, status, function, value);
    } else {
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%d\n",
              driverName, functionName, function, value);
    }
    return status;
}

/** Called when asyn clients call pasynFloat64->write().
  * This function performs actions for some parameters.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks.
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus FastCCD::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    static const char *functionName = "writeFloat64";

    /* Set the parameter and readback in the parameter library.  */
    status = setDoubleParam(function, value);

    if (function == ADAcquireTime) 
    {
      cin_ctl_set_exposure_time(&cin_ctl_port, (float)value);
      status = asynSuccess;
    } 
    else if (function == ADAcquirePeriod) 
    {
      cin_ctl_set_cycle_time(&cin_ctl_port, (float)value);
      status = asynSuccess;
    }
    else 
    {
      status = ADDriver::writeFloat64(pasynUser, value);
    }

    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();

    if(status)
    {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: error, status=%d function=%d, value=%f\n",
              driverName, functionName, status, function, value);
    }
    else
    {
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%f\n",
              driverName, functionName, function, value);
    }
    return status;
}


/**
 * Update status of detector. Meant to be run in own thread.
 */
void FastCCD::statusTask(void)
{
  // float temperature;
  // unsigned int uvalue = 0;
  unsigned int status = 0;
  double timeout = 0.0;
  static const char *functionName = "statusTask";

  while(1) {

    //Read timeout for polling freq.
    this->lock();
    timeout = mPollingPeriod;
    this->unlock();

    if (timeout != 0.0) {
      status = epicsEventWaitWithTimeout(statusEvent, timeout);
    } else {
      status = epicsEventWait(statusEvent);
    }              
    if (status == epicsEventWaitOK) {
      asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
        "%s:%s: Got status event\n",
        driverName, functionName);
    }

    try {
      cin_ctl_pwr_mon_t pwr_value;
      cin_ctl_fpga_status fpga_status;
      int pwr;
      int cin_status;
      cin_status  = cin_ctl_get_power_status(&cin_ctl_port, &pwr, &pwr_value);
      cin_status |= cin_ctl_get_cfg_fpga_status(&cin_ctl_port, &fpga_status);
    } catch (const std::string &e) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s:%s: %s\n",
        driverName, functionName, e.c_str());
      setStringParam(ADStatusMessage, e.c_str());
    }

    /* Call the callbacks to update any changes */
    this->lock();
    callParamCallbacks();
    this->unlock();
        
  } //End of loop

}

//C utility functions to tie in with EPICS

static void FastCCDStatusTaskC(void *drvPvt)
{
  FastCCD *pPvt = (FastCCD *)drvPvt;

  pPvt->statusTask();
}


/** IOC shell configuration command for Andor driver
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread
  * \param[in] stackSize The stack size for the asyn port driver thread
  * \param[in] packetBuffer The CINDATA packet buffer size
  * \param[in] imageBuffer The CINDATA image buffer size
  */
extern "C" {

int FastCCDConfig(const char *portName, int maxBuffers, size_t maxMemory, 
                  int priority, int stackSize, int packetBuffer, int imageBuffer)
{
  new FastCCD(portName, maxBuffers, maxMemory, priority, stackSize, packetBuffer, imageBuffer);
  return(asynSuccess);
}

/* Code for iocsh registration */

/* FastCCDConfig */
static const iocshArg FastCCDConfigArg0 = {"Port name", iocshArgString};
static const iocshArg FastCCDConfigArg1 = {"maxBuffers", iocshArgInt};
static const iocshArg FastCCDConfigArg2 = {"maxMemory", iocshArgInt};
static const iocshArg FastCCDConfigArg3 = {"priority", iocshArgInt};
static const iocshArg FastCCDConfigArg4 = {"stackSize", iocshArgInt};
static const iocshArg FastCCDConfigArg5 = {"packetBuffer", iocshArgInt};
static const iocshArg FastCCDConfigArg6 = {"imageBuffer", iocshArgInt};
static const iocshArg * const FastCCDConfigArgs[] =  {&FastCCDConfigArg0,
                                                       &FastCCDConfigArg1,
                                                       &FastCCDConfigArg2,
                                                       &FastCCDConfigArg3,
                                                       &FastCCDConfigArg4,
                                                       &FastCCDConfigArg5,
                                                       &FastCCDConfigArg6};

static const iocshFuncDef configFastCCD = {"FastCCDConfig", 7, FastCCDConfigArgs};
static void configFastCCDCallFunc(const iocshArgBuf *args)
{
    FastCCDConfig(args[0].sval, args[1].ival, args[2].ival,
                  args[3].ival, args[4].ival, args[5].ival,
                  args[6].ival);
}

static void FastCCDRegister(void)
{
    iocshRegister(&configFastCCD, configFastCCDCallFunc);
}

epicsExportRegistrar(FastCCDRegister);

} // extern "C"

