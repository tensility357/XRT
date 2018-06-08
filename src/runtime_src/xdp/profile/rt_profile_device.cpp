/**
 * Copyright (C) 2016-2017 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

/**
 * COPYRIGHT NOTICE
 * Copyright 2015 Xilinx Inc. All Rights Reserved.
 *
 * Author : Paul Schumacher (paul.schumacher@xilinx.com)
 *
 * SDAccel device profiling
 **/
 
#include "rt_profile_device.h"
#include "xdp/rt_singleton.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <chrono>

#define getBit(word, bit) (((word) >> bit) & 0x1)

namespace {

static const char* 
valueOrEmpty(const char* str)
{
  return str ? str : "";
}

static bool isOriginalProfiling()
{
  std::string env = valueOrEmpty(std::getenv("XCL_PLATFORM"));
  return (env.find("xilinx_adm-pcie-7v3_1ddr_1_0") != std::string::npos);
}

}

namespace XCL {

  // Constructor
  RTProfileDevice::RTProfileDevice()
    : mStartTimeNsec(0),
      NUM_TRAIN(3),
      PCIE_DELAY_OFFSET_MSEC(0.25)
  {
    mTag = 0X586C0C6C;

    mNumTraceEvents = 0;
    // NOTE: setting this to 0x80000 causes runtime crash when running
    // HW emulation on 070_max_wg_size or 079_median1
    mMaxTraceEvents = 0x40000;
    mEmuTraceMsecOneCycle = 0.0;

    mTraceSamplesThreshold = MAX_TRACE_NUMBER_SAMPLES / 4;
    mSampleIntervalMsec = 10;

    mTraceClockRateMHz = 300.0;
    mDeviceClockRateMHz = 300.0;
    mGlobalMemoryClockRateMHz = 300.0;

    // Default bit width of global memory defined at APM monitoring slaves
    mGlobalMemoryBitWidth = XPAR_AXI_PERF_MON_0_SLOT0_DATA_WIDTH;

    // Since device timestamps are in cycles and host timestamps are in msec,
    // then the slope of the line to convert from device to host timestamps
    // is in msec/cycle
    for (int i=0; i < XCL_PERF_MON_TOTAL_PROFILE; i++) {
      mTrainSlope[i] = 1000.0 / mTraceClockRateMHz;
      mTrainOffset[i] = 0.0;
    }

    memset(&mPrevTimestamp, 0, XCL_PERF_MON_TOTAL_PROFILE*sizeof(uint32_t));
  }

  // Destructor
  RTProfileDevice::~RTProfileDevice() {
    mDeviceFirstTimestamp.clear();

    // Clear queues (i.e., swap with an empty one)
    std::queue<uint64_t> empty64;
    std::queue<uint32_t> empty32;
    std::queue<uint16_t> empty16;
    for (int i=0; i < XSPM_MAX_NUMBER_SLOTS; i++) {
      std::swap(mWriteStarts[i], empty64);
      std::swap(mHostWriteStarts[i], empty64);
      std::swap(mReadStarts[i], empty64);
      std::swap(mHostReadStarts[i], empty64);
      std::swap(mWriteLengths[i], empty32);
      std::swap(mReadLengths[i], empty32);
      std::swap(mWriteBytes[i], empty16);
      std::swap(mReadBytes[i], empty16);
    }
  }

  // Log device trace results: store in queues and report events as they are completed
  void RTProfileDevice::logTrace(std::string deviceName, xclPerfMonType type,
      xclTraceResultsVector& traceVector, TraceResultVector& resultVector) {
    if (mNumTraceEvents >= mMaxTraceEvents || traceVector.mLength == 0)
      return;

    uint32_t numSlots = XCL::RTSingleton::Instance()->getProfileNumberSlots(XCL_PERF_MON_MEMORY, deviceName);
    bool isHwEmu = (XCL::RTSingleton::Instance()->getFlowMode() == XCL::RTSingleton::HW_EM);
    uint8_t flags = 0;
    uint32_t prevHostTimestamp = 0xFFFFFFFF;
    uint32_t slotID = 0;
    uint32_t timestamp = 0;
    uint64_t deviceStartTimestamp = 0;
    uint64_t hostTimestampNsec = 0;
    uint64_t startTime = 0;
    double y1, y2, x1, x2;
    DeviceTrace kernelTrace;
    
    XDP_LOG("[rt_device_profile] Logging %u device trace samples (total = %ld)...\n",
        traceVector.mLength, mNumTraceEvents);
    mNumTraceEvents += traceVector.mLength;

    // Find and set minimum timestamp in case of multiple Kernels
    if(isHwEmu) {
      uint64_t minHostTimestampNsec = traceVector.mArray[0].HostTimestamp;
      for (int i=0; i < traceVector.mLength; i++) {
        if (traceVector.mArray[i].HostTimestamp < minHostTimestampNsec)
          minHostTimestampNsec = traceVector.mArray[i].HostTimestamp;
      }
      getTimestampNsec(minHostTimestampNsec);
    }
    else {
      if (traceVector.mLength >= 8192)
        xrt::message::send(xrt::message::severity_level::WARNING,
"Trace FIFO is full because of too many events. Timeline trace could be incomplete. \
Please use 'coarse' option for data transfer trace or turn off Stall profiling");
    }
  
    //
    // Parse recently offloaded trace results
    //
    for (int i=0; i < traceVector.mLength; i++) {
      xclTraceResults trace = traceVector.mArray[i];
      XDP_LOG("[rt_device_profile] Parsing trace sample %d...\n", i);

      // ***************
      // Clock Training
      // ***************
      if (isHwEmu) {
        timestamp = trace.Timestamp + mPrevTimestamp[type];
        if (trace.Overflow == 1)
          timestamp += LOOP_ADD_TIME;
        mPrevTimestamp[type] = timestamp;

        if (trace.HostTimestamp == prevHostTimestamp && trace.Timestamp == 1) {
          XDP_LOG("[rt_device_profile] Ignoring host timestamp: 0x%X\n",
                  trace.HostTimestamp);
          continue;
        }
        hostTimestampNsec = getTimestampNsec(trace.HostTimestamp);
        XDP_LOG("[rt_device_profile] Timestamp pair: Device: 0x%X, Host: 0x%X\n",
                timestamp, hostTimestampNsec);
        prevHostTimestamp = trace.HostTimestamp;
      }
      else {
        // for hw first two packets are for clock training
        // 1000 is to account for delay in sending from host
        // TODO: Calculate the delay instead of hard coding
        if (i == 0) {
          y1 = static_cast <double> (trace.HostTimestamp) + 1000;
          x1 = static_cast <double> (trace.Timestamp);
          continue;
        }
        if (i == 1) {
          y2 = static_cast <double> (trace.HostTimestamp) + 1000;
          x2 = static_cast <double> (trace.Timestamp);
          mTrainSlope[type] = (y2 - y1) / (x2 - x1);
          mTrainOffset[type] = y2 - mTrainSlope[type] * x2;
          trainDeviceHostTimestamps(deviceName, type);
        }
        if (trace.Overflow == 1)
          trace.Timestamp += LOOP_ADD_TIME_SPM;
        timestamp = trace.Timestamp;
        if (trace.TraceID >= 64 && trace.TraceID <= 544)
          slotID = ((trace.TraceID - 64) / 16);
        else
          // SPM Trace IDs (Slots 0-30)
          if (trace.TraceID >= 2 && trace.TraceID <= 61)
            slotID = trace.TraceID/2;
          else
            // Unsupported
            continue;
      }
      uint32_t s = slotID;

      if (isHwEmu) {
        if (trace.TraceID < 61) {
        s = trace.TraceID / 2;
        flags = trace.EventFlags;
        XDP_LOG("[rt_device_profile] slot %d event flags = %s @ timestamp %d\n",
              s, dec2bin(flags, 7).c_str(), timestamp);
        
        // Write start
        if (getBit(flags, XAPM_WRITE_FIRST)) {
          mWriteStarts[s].push(timestamp);
          mHostWriteStarts[s].push(hostTimestampNsec);
        }
  
        // Write end
        // NOTE: does not support out-of-order tranx
        if (getBit(flags, XAPM_WRITE_LAST)) {
          if (mWriteStarts[s].empty()) {
            XDP_LOG("[rt_device_profile] WARNING: Found write end with write start queue empty @ %d\n", timestamp);
            continue;
          }
  
          uint64_t startTime = mWriteStarts[s].front();
          uint64_t hostStartTime = mHostWriteStarts[s].front();  
          mWriteStarts[s].pop();
          mHostWriteStarts[s].pop();
  
          // Add write trace class to vector
          DeviceTrace writeTrace;
          writeTrace.SlotNum = s;
          writeTrace.Type = "Write";
          writeTrace.StartTime = startTime;
          writeTrace.EndTime = timestamp;
          writeTrace.Start = hostStartTime / 1e6;
          writeTrace.End = hostTimestampNsec / 1e6;
          if (writeTrace.Start == writeTrace.End) writeTrace.End += mEmuTraceMsecOneCycle;
          writeTrace.BurstLength = timestamp - startTime + 1;
  
          // Only report tranx that make sense
          if (writeTrace.End >= writeTrace.Start) {
            writeTrace.TraceStart = hostStartTime / 1e6;
            resultVector.push_back(writeTrace);
          }
        }
  
        // Read start
        if (getBit(flags, XAPM_READ_FIRST)) {
          mReadStarts[s].push(timestamp);
          mHostReadStarts[s].push(hostTimestampNsec);
        }
  
        // Read end
        // NOTE: does not support out-of-order tranx
        if (getBit(flags, XAPM_READ_LAST)) {
          if (mReadStarts[s].empty()) {
            XDP_LOG("[rt_device_profile] WARNING: Found read end with read start queue empty @ %d\n", timestamp);
            continue;
          }

          uint64_t startTime = mReadStarts[s].front();
          uint64_t hostStartTime = mHostReadStarts[s].front();
          mReadStarts[s].pop();
          mHostReadStarts[s].pop();
  
          // Add read trace class to vector
          DeviceTrace readTrace;
          readTrace.SlotNum = s;
          readTrace.Type = "Read";
          readTrace.StartTime = startTime;
          readTrace.EndTime = timestamp;
          readTrace.Start = hostStartTime / 1e6;
          readTrace.End = hostTimestampNsec / 1e6;
          // Single Burst
          if (readTrace.Start == readTrace.End) readTrace.End += mEmuTraceMsecOneCycle;
          readTrace.BurstLength = timestamp - startTime + 1;
  
          // Only report tranx that make sense
          if (readTrace.End >= readTrace.Start) {
            readTrace.TraceStart = hostStartTime / 1e6;
            resultVector.push_back(readTrace);
          }
        }
        }
        else if (trace.TraceID >= 64 && trace.TraceID <= 94) {
          uint32_t cuEvent = trace.EventFlags & XSAM_TRACE_CU_MASK;
          s = trace.TraceID - 64;
          // Common Params for all event types
          kernelTrace.SlotNum = s;
          kernelTrace.Name = "OCL Region";
          kernelTrace.Kind = DeviceTrace::DEVICE_KERNEL;
          kernelTrace.EndTime = timestamp;
          kernelTrace.End = hostTimestampNsec / 1e6;
          kernelTrace.BurstLength = 0;
          kernelTrace.NumBytes = 0;
          if (cuEvent) {
            if (mAccelMonStartedEvents[s] & XSAM_TRACE_CU_MASK) {
              kernelTrace.Type = "Kernel";
              startTime = mAccelMonCuHostTime[s];
              kernelTrace.StartTime = mAccelMonCuTime[s];
              kernelTrace.Start = mAccelMonCuHostTime[s] / 1e6;
              resultVector.push_back(kernelTrace);
              // Divide by 2 just to be safe
              mEmuTraceMsecOneCycle = (kernelTrace.End - kernelTrace.Start) / (2 *(kernelTrace.EndTime - kernelTrace.StartTime));
            }
            else {
              mAccelMonCuHostTime[s] = hostTimestampNsec;
              mAccelMonCuTime[s] = timestamp;
            }
            mAccelMonStartedEvents[s] ^= XSAM_TRACE_CU_MASK;
          }
        }
        else continue;
      } // If Hw Emu
      else {
        // SAM Trace
        if (trace.TraceID >= 64) {
          uint32_t cuEvent       = trace.TraceID & XSAM_TRACE_CU_MASK;
          uint32_t stallIntEvent = trace.TraceID & XSAM_TRACE_STALL_INT_MASK;
          uint32_t stallStrEvent = trace.TraceID & XSAM_TRACE_STALL_STR_MASK;
          uint32_t stallExtEvent = trace.TraceID & XSAM_TRACE_STALL_EXT_MASK;
          // Common Params for all event types
          kernelTrace.SlotNum = s;
          kernelTrace.Name = "OCL Region";
          kernelTrace.Kind = DeviceTrace::DEVICE_KERNEL;
          kernelTrace.EndTime = timestamp;
          kernelTrace.BurstLength = 0;
          kernelTrace.NumBytes = 0;
          kernelTrace.End = convertDeviceToHostTimestamp(timestamp, type, deviceName);
          if (cuEvent) {
            if (mAccelMonStartedEvents[s] & XSAM_TRACE_CU_MASK) {
              kernelTrace.Type = "Kernel";
              startTime = mAccelMonCuTime[s];
              kernelTrace.StartTime = startTime;
              kernelTrace.Start = convertDeviceToHostTimestamp(startTime, type, deviceName);
              kernelTrace.TraceStart = kernelTrace.Start;
              resultVector.insert(resultVector.begin(), kernelTrace);
            }
            else {
              mAccelMonCuTime[s] = timestamp;
            }
          }
          if (stallIntEvent) {
            if (mAccelMonStartedEvents[s] & XSAM_TRACE_STALL_INT_MASK) {
              kernelTrace.Type = "Intra-Kernel Dataflow Stall";
              startTime = mAccelMonStallIntTime[s];
              kernelTrace.StartTime = startTime;
              kernelTrace.Start = convertDeviceToHostTimestamp(startTime, type, deviceName);
              kernelTrace.TraceStart = kernelTrace.Start;
              resultVector.push_back(kernelTrace);
            }
            else {
              mAccelMonStallIntTime[s] = timestamp;
            }
          }
          if (stallStrEvent) {
            if (mAccelMonStartedEvents[s] & XSAM_TRACE_STALL_STR_MASK) {
              kernelTrace.Type = "Inter-Kernel Pipe Stall";
              startTime = mAccelMonStallStrTime[s];
              kernelTrace.StartTime = startTime;
              kernelTrace.Start = convertDeviceToHostTimestamp(startTime, type, deviceName);
              kernelTrace.TraceStart = kernelTrace.Start;
              resultVector.push_back(kernelTrace);
            }
            else {
              mAccelMonStallStrTime[s] = timestamp;
            }
          }
          if (stallExtEvent) {
            if (mAccelMonStartedEvents[s] & XSAM_TRACE_STALL_EXT_MASK) {
              kernelTrace.Type = "External Memory Stall";
              startTime = mAccelMonStallExtTime[s];
              kernelTrace.StartTime = startTime;
              kernelTrace.Start = convertDeviceToHostTimestamp(startTime, type, deviceName);
              kernelTrace.TraceStart = kernelTrace.Start;
              resultVector.push_back(kernelTrace);
            }
            else {
              mAccelMonStallExtTime[s] = timestamp;
            }
          }
          // Update Events
          mAccelMonStartedEvents[s] ^= (trace.TraceID & 0xf);
          mAccelMonLastTranx[s] = timestamp;
        }
        else // SPM Trace
        if (IS_READ(trace.TraceID)) {
          if (trace.EventType == XCL_PERF_MON_START_EVENT) {
            mReadStarts[s].push(timestamp);
          }
          else if (trace.EventType == XCL_PERF_MON_END_EVENT) {
             if (trace.Reserved == 1) {
              startTime = timestamp;
             }
             else {
              if(mReadStarts[s].empty()) {
                startTime = timestamp;
              } else {
                startTime = mReadStarts[s].front();
                mReadStarts[s].pop();
              }
             }
            DeviceTrace readTrace;
            readTrace.SlotNum = slotID;
            readTrace.Type = "Read";
            readTrace.StartTime = startTime;
            readTrace.EndTime = timestamp;
            readTrace.BurstLength = timestamp - startTime + 1;
            readTrace.Start = convertDeviceToHostTimestamp(startTime, type, deviceName);
            readTrace.End = convertDeviceToHostTimestamp(timestamp, type, deviceName);
            resultVector.push_back(readTrace);
            mPerfMonLastTranx[slotID] = timestamp;
          }
        }
        else
        if (IS_WRITE(trace.TraceID)) {
          if (trace.EventType == XCL_PERF_MON_START_EVENT) {
            mWriteStarts[s].push(timestamp);
          }
          else if (trace.EventType == XCL_PERF_MON_END_EVENT) {
            if (trace.Reserved == 1) {
              startTime = timestamp;
            }
            else {
              if(mWriteStarts[s].empty()) {
                startTime = timestamp;
              } else {
                startTime = mWriteStarts[s].front();
                mWriteStarts[s].pop();
              }
            }
            DeviceTrace writeTrace;
            writeTrace.SlotNum = slotID;
            writeTrace.Type = "Write";
            writeTrace.StartTime = startTime;
            writeTrace.EndTime = timestamp;
            writeTrace.BurstLength = timestamp - startTime + 1;
            writeTrace.Start = convertDeviceToHostTimestamp(startTime, type, deviceName);
            writeTrace.End = convertDeviceToHostTimestamp(timestamp, type, deviceName);
            resultVector.push_back(writeTrace);
            mPerfMonLastTranx[slotID] = timestamp;
          }
        }
      } // Else Hardware
    } // for i
    
    // Try to approximate CU Ends from data transnfers
    if(!isHwEmu) {
      std::string cuPortName, cuNameSAM, cuNameSPM;
      auto rts = XCL::RTSingleton::Instance();
      for (int i = 0; i < XSAM_MAX_NUMBER_SLOTS; i++) {
        if (mAccelMonStartedEvents[i] & XSAM_TRACE_CU_MASK) {
          kernelTrace.SlotNum = i;
          kernelTrace.Name = "OCL Region";
          kernelTrace.Type = "Kernel";
          kernelTrace.Kind = DeviceTrace::DEVICE_KERNEL;
          kernelTrace.StartTime = mAccelMonCuTime[i];
          kernelTrace.Start = convertDeviceToHostTimestamp(kernelTrace.StartTime, type, deviceName);
          kernelTrace.BurstLength = 0;
          kernelTrace.NumBytes = 0;
          uint64_t lastTimeStamp = 0;
          rts->getProfileSlotName(XCL_PERF_MON_ACCEL, deviceName, i, cuNameSAM);
          for (int j = 0; j < XSPM_MAX_NUMBER_SLOTS; j++) {
            rts->getProfileSlotName(XCL_PERF_MON_MEMORY, deviceName, j, cuPortName);
            cuNameSPM = cuPortName.substr(0, cuPortName.find_first_of("/"));
            if (cuNameSAM == cuNameSPM && lastTimeStamp < mPerfMonLastTranx[j])
              lastTimeStamp = mPerfMonLastTranx[j];
          }
          if (lastTimeStamp < mAccelMonLastTranx[i])
            lastTimeStamp = mAccelMonLastTranx[i];
          if (lastTimeStamp) {
            xrt::message::send(xrt::message::severity_level::WARNING,
            "Incomplete CU profile trace detected. Timeline trace will have approximate CU End");
            kernelTrace.EndTime = lastTimeStamp;
            kernelTrace.End = convertDeviceToHostTimestamp(kernelTrace.EndTime, type, deviceName);
            // Insert is needed in case there are only stalls
            resultVector.insert(resultVector.begin(), kernelTrace);
          }
        }
      }
    }
    // Clear vectors
    std::fill_n(mAccelMonStartedEvents,XSAM_MAX_NUMBER_SLOTS,0);
    mDeviceTrainVector.clear();
    mHostTrainVector.clear();

    XDP_LOG("[rt_device_profile] Done logging device trace samples\n");
  }

  // ****************
  // Helper functions
  // ****************

  // Get slot name
  void RTProfileDevice::getSlotName(int slotnum, std::string& slotName) const {
    if (slotnum < 0 || slotnum >= XAPM_MAX_NUMBER_SLOTS) {
      slotName = "Null";
      return;
    }

    if (isOriginalProfiling()) {
      switch (slotnum) {
      case 0:
        slotName = XPAR_AXI_PERF_MON_0_SLOT1_NAME;
        break;
      case 1:
        slotName = XPAR_AXI_PERF_MON_0_SLOT0_NAME;
        break;
      default:
        slotName = "Null";
        break;
      }
      return;
    }

    switch (slotnum) {
    case 0:
      slotName = XPAR_AXI_PERF_MON_0_SLOT0_NAME;
      break;
    case 1:
      slotName = XPAR_AXI_PERF_MON_0_SLOT1_NAME;
      break;
    case 2:
      slotName = XPAR_AXI_PERF_MON_0_SLOT2_NAME;
      break;
    case 3:
      slotName = XPAR_AXI_PERF_MON_0_SLOT3_NAME;
      break;
    case 4:
      slotName = XPAR_AXI_PERF_MON_0_SLOT4_NAME;
      break;
    case 5:
      slotName = XPAR_AXI_PERF_MON_0_SLOT5_NAME;
      break;
    case 6:
      slotName = XPAR_AXI_PERF_MON_0_SLOT6_NAME;
      break;
    case 7:
      slotName = XPAR_AXI_PERF_MON_0_SLOT7_NAME;
      break;
    default:
      slotName = "Null";
      break;
    }
  }

  // Get slot kind
  DeviceTrace::e_device_kind
  RTProfileDevice::getSlotKind(std::string& slotName) const {
    if (slotName == "Host") return DeviceTrace::DEVICE_BUFFER;
    return DeviceTrace::DEVICE_KERNEL;
  }

  // Convert binary string to decimal
  uint32_t RTProfileDevice::bin2dec(std::string str, int start, int number) {
    return bin2dec(str.c_str(), start, number);
  }

  // Convert binary char * to decimal
  uint32_t RTProfileDevice::bin2dec(const char* ptr, int start, int number) {
    const char* temp_ptr = ptr + start;
    uint32_t value = 0;
    int i = 0;

    do {
      if (*temp_ptr != '0' && *temp_ptr!= '1')
        return value;
      value <<= 1;
      if(*temp_ptr=='1')
        value += 1;
      i++;
      temp_ptr++;
    } while (i < number);

    return value;
  }

  // Convert decimal to binary string
  // NOTE: length of string is always sizeof(uint32_t) * 8
  std::string RTProfileDevice::dec2bin(uint32_t n) {
    char result[(sizeof(uint32_t) * 8) + 1];
    unsigned index = sizeof(uint32_t) * 8;
    result[index] = '\0';

    do result[ --index ] = '0' + (n & 1);
    while (n >>= 1);

    for (int i=index-1; i >= 0; --i)
        result[i] = '0';

    return std::string( result );
  }

  // Convert decimal to binary string of length bits
  std::string RTProfileDevice::dec2bin(uint32_t n, unsigned bits) {
#ifndef _WINDOWS
// TODO: Windows build support
// runtime array size is not supported
	  char result[bits + 1];
#else
// temporarily give a random number
	  char result[100];
#endif
	  unsigned index = bits;
	  result[index] = '\0';

	  do result[ --index ] = '0' + (n & 1);
	  while (n >>= 1);

	  for (int i=index-1; i >= 0; --i)
		result[i] = '0';

	  return std::string( result );
  }

  // Complete training to convert device timestamp to host time domain
  // NOTE: see description of PTP @ http://en.wikipedia.org/wiki/Precision_Time_Protocol
  void RTProfileDevice::trainDeviceHostTimestamps(std::string deviceName, xclPerfMonType type) {
    using namespace std::chrono;
    typedef duration<uint64_t, std::ratio<1, 1000000000>> duration_ns;
    duration_ns time_span =
        duration_cast<duration_ns>(high_resolution_clock::now().time_since_epoch());
    uint64_t currentOffset = static_cast<uint64_t>(xrt::time_ns());
    uint64_t currentTime = time_span.count();
    mTrainProgramStart[type] = static_cast<double>(currentTime - currentOffset);
  }

  // Convert device timestamp to host time domain (in msec)
  double RTProfileDevice::convertDeviceToHostTimestamp(uint64_t deviceTimestamp, xclPerfMonType type,
      const std::string& deviceName) { 
    // Return y = m*x + b with b relative to program start
    return (mTrainSlope[type] * (double)deviceTimestamp)/1e6 + (mTrainOffset[type]-mTrainProgramStart[type])/1e6;
  }

}


