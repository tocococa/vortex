#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>
#include <assert.h>

#ifdef USE_VLSIM
#include "vlsim/fpga.h"
#else
#include <opae/fpga.h>
#endif

#include <VX_config.h>
#include "vx_scope.h"
#include "vortex_afu.h"
#include "scope-defs.h"

#define SCOPE_FRAME_WIDTH 1768

#define CHECK_RES(_expr)                            \
   do {                                             \
     fpga_result res = _expr;                       \
     if (res == FPGA_OK)                            \
       break;                                       \
     printf("OPAE Error: '%s' returned %d, %s!\n",  \
            #_expr, (int)res, fpgaErrStr(res));     \
     return -1;                                     \
   } while (false)

#define MMIO_SCOPE_READ     (AFU_IMAGE_MMIO_SCOPE_READ * 4)
#define MMIO_SCOPE_WRITE    (AFU_IMAGE_MMIO_SCOPE_WRITE * 4)

#define CMD_GET_VALID   0 
#define CMD_GET_DATA    1 
#define CMD_GET_WIDTH   2
#define CMD_GET_COUNT   3 
#define CMD_SET_DELAY   4
#define CMD_SET_STOP    5
#define CMD_GET_OFFSET  6

static constexpr int num_signals = sizeof(scope_signals) / sizeof(scope_signal_t);

constexpr int calcFrameWidth(int index = 0) {
    return (index < num_signals) ? (scope_signals[index].width + calcFrameWidth(index + 1)) : 0;
}

static constexpr int fwidth = calcFrameWidth();

uint64_t print_clock(std::ofstream& ofs, uint64_t delta, uint64_t timestamp) {
    while (delta != 0) {
        ofs << '#' << timestamp++ << std::endl;
        ofs << "b0 0" << std::endl;
        ofs << '#' << timestamp++ << std::endl;
        ofs << "b1 0" << std::endl;
        --delta;
    }
    return timestamp;
}

int vx_scope_start(fpga_handle hfpga, uint64_t delay) {    
    if (nullptr == hfpga)
        return -1;  
    
    if (delay != uint64_t(-1)) {
        // set start delay
        uint64_t cmd_delay = ((delay << 3) | CMD_SET_DELAY);
        CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, cmd_delay));    
        std::cout << "scope start delay: " << delay << std::endl;
    }

    return 0;
}

int vx_scope_stop(fpga_handle hfpga, uint64_t delay) {    
    if (nullptr == hfpga)
        return -1;
    
    if (delay != uint64_t(-1)) {
        // stop recording
        uint64_t cmd_stop = ((delay << 3) | CMD_SET_STOP);
        CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, cmd_stop));
        std::cout << "scope stop delay: " << delay << std::endl;
    }

    std::ofstream ofs("vx_scope.vcd");

    ofs << "$version Generated by Vortex Scope $end" << std::endl;
    ofs << "$timescale 1 ns $end" << std::endl;
    ofs << "$scope module TOP $end" << std::endl;
    ofs << "$var reg 1 0 clk $end" << std::endl;

    for (int i = 0; i < num_signals; ++i) {
        ofs << "$var reg " << scope_signals[i].width << " " << (i+1) << " " << scope_signals[i].name << " $end" << std::endl;
    }

    ofs << "$upscope $end" << std::endl;
    ofs << "enddefinitions $end" << std::endl;
    
    uint64_t frame_width, max_frames, data_valid, offset, delta;   
    uint64_t timestamp = 0;
    uint64_t frame_offset = 0;
    uint64_t frame_no = 0;  
    int signal_id = 0;
    int signal_offset = 0; 

    // wait for recording to terminate
    CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_VALID));
    do {        
        CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &data_valid));        
        if (data_valid)
            break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (true);

    std::cout << "scope trace dump begin..." << std::endl;    

    // get frame width
    CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_WIDTH));
    CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &frame_width));
    std::cout << "scope::frame_width=" << std::dec << frame_width << std::endl;  

    if (fwidth != (int)frame_width) {   
        std::cerr << "invalid frame_width: expecting " << std::dec << fwidth << "!" << std::endl;
        std::abort();
    }

    // get max frames
    CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_COUNT));
    CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &max_frames));
    std::cout << "scope::max_frames=" << std::dec << max_frames << std::endl;    

    // get offset    
    CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_OFFSET));    
    CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &offset));

    // get data
    CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_DATA));

    // print clock header
    CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &delta));
    timestamp = print_clock(ofs, offset + delta + 2, timestamp);
    signal_id = num_signals;

    std::vector<char> signal_data(frame_width+1);

    do {
        if (frame_no == (max_frames-1)) {
            // verify last frame is valid
            CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_VALID));
            CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &data_valid));  
            assert(data_valid == 1);
            CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_DATA));
        }

        // read next data words
        uint64_t word;
        CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &word));
        
        do {          
            int signal_width = scope_signals[signal_id-1].width;
            int word_offset = frame_offset % 64;

            signal_data[signal_width - signal_offset - 1] = ((word >> word_offset) & 0x1) ? '1' : '0';

            ++signal_offset;
            ++frame_offset;

            if (signal_offset == signal_width) {
                signal_data[signal_width] = 0; // string null termination
                ofs << 'b' << signal_data.data() << ' ' << signal_id << std::endl;
                signal_offset = 0;            
                --signal_id;
            }

            if (frame_offset == frame_width) {   
                assert(0 == signal_offset);   
                frame_offset = 0;
                ++frame_no;

                if (frame_no != max_frames) {    
                    // print clock header
                    CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &delta));
                    timestamp = print_clock(ofs, delta + 1, timestamp);            
                    signal_id = num_signals;
                    //std::cout << "*** " << frame_no << " frames, timestamp=" << timestamp << std::endl;
                }                     
            }
            
        } while ((frame_offset % 64) != 0);

    } while (frame_no != max_frames);

    std::cout << "scope trace dump done! - " << (timestamp/2) << " cycles" << std::endl;

    // verify data not valid
    CHECK_RES(fpgaWriteMMIO64(hfpga, 0, MMIO_SCOPE_WRITE, CMD_GET_VALID));
    CHECK_RES(fpgaReadMMIO64(hfpga, 0, MMIO_SCOPE_READ, &data_valid));  
    assert(data_valid == 0);

    return 0;
}