//
// Copyright (c) 2012, University of Erlangen-Nuremberg
// Copyright (c) 2012, Siemens AG
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

/*
 * Tool that benchmarks the achievable memory bandwidth throughput using OpenCL
 * memcpy API calls.
 */

#include <algorithm>
#include <sstream>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hipacc_ocl.hpp"


void usage(char **argv) {
    fprintf(stderr, "Usage: %s [-h] [-d ACC|CPU|GPU|ALL] [-p AMD|APPLE|ARM|INTEL|NVIDIA|ALL] -s <memory_size>\n", argv[0]);
}


int main(int argc, char *argv[]) {
    int option = 0;
    cl_device_type device_type = CL_DEVICE_TYPE_ALL;
    cl_platform_name platform_name = ALL;
    size_t memory_size = 64*(1 << 20);      //64 M

    // scan command-line options
    while ((option = getopt(argc, (char * const *)argv, "hd:p:s:")) != -1) {
        switch (option) {
            case 'h':
                fprintf(stderr, "Compile OpenCL kernel for given Platform and/or device type.\n");
                usage(argv);
                exit(EXIT_SUCCESS);
            case 'd':
                if (strncmp(optarg, "ACC", 3) == 0) device_type = CL_DEVICE_TYPE_ACCELERATOR;
                else if (strncmp(optarg, "CPU", 3) == 0) device_type = CL_DEVICE_TYPE_CPU;
                else if (strncmp(optarg, "GPU", 3) == 0) device_type = CL_DEVICE_TYPE_GPU;
                else if (strncmp(optarg, "ALL", 3) == 0) device_type = CL_DEVICE_TYPE_ALL;
                else fprintf(stderr, "Unknown device type '%s', using 'ALL' as default ...\n", optarg);
                break;
            case 'p':
                if (strncmp(optarg, "AMD", 3) == 0) platform_name = AMD;
                else if (strncmp(optarg, "APPLE", 3) == 0) platform_name = APPLE;
                else if (strncmp(optarg, "ARM", 3) == 0) platform_name = ARM;
                else if (strncmp(optarg, "INTEL", 3) == 0) platform_name = INTEL;
                else if (strncmp(optarg, "NVIDIA", 3) == 0) platform_name = NVIDIA;
                else fprintf(stderr, "Unknown platform name '%s', using 'ALL' as default ...\n", optarg);
                break;
            case 's':
                std::istringstream (optarg) >> memory_size;
                break;
            default: /* '?' */
                fprintf(stderr, "Wrong call syntax!\n");
                usage(argv);
                exit(EXIT_FAILURE);
        }
    }

    // initialize all devices
    hipaccInitPlatformsAndDevices(device_type, platform_name);
    std::vector<cl_device_id> devices_all = hipaccGetAllDevices();
    hipaccCreateContextsAndCommandQueues(true);

    // TODO: add support for; currently only DEVICE_TO_DEVICE is implemented
    // a) memcpyKind : DEVICE_TO_HOST, HOST_TO_DEVICE, DEVICE_TO_DEVICE
    // b) memoryMode : PAGEABLE, PINNED
    // c) accessMode : MAPPED, DIRECT

    // allocate host memory
    unsigned char *host_idata = (unsigned char *)malloc(memory_size);

    // initialize the memory
    for (size_t i=0; i < memory_size/sizeof(unsigned char); ++i) {
        host_idata[i] = (unsigned char) (i & 0xff);
    }

    // allocate device input and output memory
    HipaccImage dev_idata = hipaccCreateBuffer<unsigned char>(NULL, memory_size, 1);
    HipaccImage dev_odata = hipaccCreateBuffer<unsigned char>(NULL, memory_size, 1);

    std::cout << std::endl << "Bandwidth test, memory size [MB]: " << memory_size/(1024*1024) << std::endl;
    for (size_t num_device=0; num_device<devices_all.size(); ++num_device) {
        // copy data to device
        hipaccWriteMemory<unsigned char>(dev_idata, host_idata, num_device);

        // get time in ms
        double time = hipaccCopyBufferBenchmark(dev_idata, dev_odata, num_device);
        // time in s
        time = time/1000;

        // calculate bandwidth in MB/s
        // this is for kernels that read and write global memory simultaneously
        // obtained throughput for unidirectional block copies will be 1/2 of this #
        float bandwidth_MBs = 2.0f * ((double)memory_size)/(time * (double)(1 << 20));

        // print statistic
        std::cout << "Device number: " << num_device << std::endl;
        std::cout << "Bandwidth [MB/s]: " << bandwidth_MBs << std::endl;
        std::cout << "Bandwidth [GB/s]: " << bandwidth_MBs/1024 << std::endl;
    }

    // clean up memory on host
    free(host_idata);

    // clean up memory on device
    hipaccReleaseMemory(dev_idata);
    hipaccReleaseMemory(dev_odata);

    return EXIT_SUCCESS;
}

