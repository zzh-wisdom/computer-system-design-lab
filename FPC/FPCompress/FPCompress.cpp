/*******************************************************************************
* Copyright (c) 2012-2014, The Microsystems Design Labratory (MDL)
* Department of Computer Science and Engineering, The Pennsylvania State University
* All rights reserved.
* 
* This source code is part of NVMain - A cycle accurate timing, bit accurate
* energy simulator for both volatile (e.g., DRAM) and non-volatile memory
* (e.g., PCRAM). The source code is free and you can redistribute and/or
* modify it by providing that the following conditions are met:
* 
*  1) Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
* 
*  2) Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the documentation
*     and/or other materials provided with the distribution.
* 
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
* 
* Author list: 
*   Matt Poremba    ( Email: mrp5060 at psu dot edu 
*                     Website: http://www.cse.psu.edu/~poremba/ )
*******************************************************************************/

#include "DataEncoders/FPCompress/FPCompress.h"

#include <iostream>

using namespace NVM;
using namespace std;

// 判断写入数据是否是0的代码
static inline bool pattern_zero(int64_t x) {
    return x == 0;
}

// 判断写入数据是否是8bit符号位填充的代码
static inline bool pattern_one(int64_t x) {
    return (x >> 7) == -1 || (x >> 7) == 0;
}

// 判断写入数据是否是16bit符号位填充的代码
static inline bool pattern_two(int64_t x) {
    return (x >> 15) == -1 || (x >> 15) == 0;
}

// 判断写入数据是否是32bit符号位填充
static inline bool pattern_three(int64_t x) {
    return (x >> 31) == -1 || (x >> 31) == 0;
}

// 判断写入数据低半字是否是32bit填充
static inline bool pattern_four(int64_t x) {
    return (x << 32) == 0;
}

// Two half-words. each a byte sign-extended
static inline bool pattern_five(int64_t x) {
    return  (((x >> (32 + 15)) == -1 || (x >> (32 + 15)) == 0)) &&
            (((x >> 15) & 0x1ffff) == 0x1ffff || ((x >> 15) & 0x1ffff) == 0);
}

// Word consisting if four repeated double bytes
static inline bool pattern_six(int64_t x) {
    int64_t repeated_double_bytes = x & 0xffff;
    int64_t expect = 0;

    expect |= repeated_double_bytes;

    expect <<= 16;
    expect |= repeated_double_bytes;

    expect <<= 16;
    expect |= repeated_double_bytes;

    expect <<= 16;
    expect |= repeated_double_bytes;

    return expect == x;
}

// uncompressed word
static inline bool pattern_seven(int64_t x) {
    return true;
}

FPCompress::FPCompress( )
{
    data_strip = 0;
    
    /* Clear statistics */
    p1 = 0;
    p2 = 0;
    p3 = 0;
    p4 = 0;
    p5 = 0;
    p6 = 0;
    p7 = 0;
}

FPCompress::~FPCompress( )
{
    /*
     *  Nothing to do here. We do not own the *config pointer, so
     *  don't delete that.
     */
}

void FPCompress::SetConfig( Config *config, bool /*createChildren*/ )
{
    // Params *params = new Params( );
    // params->SetParams( config );
    // SetParams( params );

    // /* Cache granularity size. */
    // fpSize = config->GetValue( "FlipNWriteGranularity" );

    // /* Some default size if the parameter is not specified */
    // if( fpSize == -1 )
    //     fpSize = 32; 
}

void FPCompress::RegisterStats( )
{
    AddStat(p0);
    AddStat(p1);
    AddStat(p2);
    AddStat(p3);
    AddStat(p4);
    AddStat(p5);
    AddStat(p6);
    AddStat(p7);
    // AddStat(bitsFlipped);
    // AddStat(bitCompareSwapWrites);
    // AddUnitStat(flipNWriteReduction, "%");
}

ncycle_t FPCompress::Read( NVMainRequest* /*request*/ )
{
    ncycle_t rv = 0;

    // TODO: Add some energy here

    return rv;
}

ncycle_t FPCompress::Write( NVMainRequest *request ) 
{
    ncycle_t rv = 0;

    NVMDataBlock& newData = request->data;
    NVMDataBlock& oldData = request->oldData;
	
    // return 0;
    uint64_t byte_size = 0;
    uint64_t bit_off = 0;
    uint64_t old_data = 0;
    uint64_t new_data = 0;

    int64_t *ptr = (int64_t*)(newData.rawData);
    //newData.SetSize(64);
    //oldData.SetSize(64);
    //return 0;
    while(byte_size < newData.GetSize()) {
        old_data = *ptr;

        if(pattern_zero(old_data)) {
            new_data = 0;
            fill_data(oldData.rawData, bit_off, new_data);
            p0++;
            bit_off += 3;
        } else if(pattern_one(old_data)) {
            new_data = old_data & 0xff;
            new_data |= (0x1 << 8);
            fill_data(oldData.rawData, bit_off, new_data);
            p1++;
            bit_off += 3 + 8;
        } else if(pattern_two(old_data)) {
            new_data = old_data & 0xffff;
            new_data |= (0x2 << 16);
            fill_data(oldData.rawData, bit_off, new_data);
            p2++;
            bit_off += 3 + 16;
        } else if(pattern_three(old_data)) {
            new_data = old_data & 0xffffffff;
            new_data |= ((uint64_t)0x3 << 32);
            fill_data(oldData.rawData, bit_off, new_data);
            p3++;
            bit_off += 3 + 32;
        } else if(pattern_four(old_data)) {
            new_data = old_data >> 32;
            new_data |= ((uint64_t)0x4 << 32);
            fill_data(oldData.rawData, bit_off, new_data);
            p4++;
            bit_off += 3 + 32;
        } else if(pattern_five(old_data)) {
            new_data = old_data & 0xffff;
            new_data |= ((old_data >> 32) & 0xffff) << 16;
            new_data |= ((uint64_t)0x5 << 32);
            fill_data(oldData.rawData, bit_off, new_data);
            p5++;
            bit_off += 3 + 32;
        } else if(pattern_six(old_data)) {
            new_data = old_data & 0xffff;
            new_data |= ((uint64_t)0x6 << 16);
            fill_data(oldData.rawData, bit_off, new_data);
            p6++;
            bit_off += 3 + 16;
        } else if(pattern_seven(old_data)) { // uncompressed
            new_data = old_data & 0xffffffff;
            fill_data(oldData.rawData, bit_off, new_data);
            bit_off += 32;
            new_data = old_data >> 32;
            new_data |= ((uint64_t)0x7 << 32);
            fill_data(oldData.rawData, bit_off, new_data);
            p6++;
            bit_off += 3 + 16;
        }

        // 下一个dword
        ptr++; 
        byte_size += 8;
    }

    
    return rv;
}

void FPCompress::fill_data(uint8_t *address, uint64_t bit_off, uint64_t data) {
    uint64_t *ptr = (uint64_t*)(address + (bit_off / 8));
    uint64_t new_data = *ptr;
    uint64_t valid_bit_len = bit_off - (bit_off / 8) * 8;
    // 将高bit位值为0
    new_data = (new_data << (64 - valid_bit_len)) >> (64 - valid_bit_len);
    new_data |= (data << valid_bit_len);

    *ptr = new_data;
}

