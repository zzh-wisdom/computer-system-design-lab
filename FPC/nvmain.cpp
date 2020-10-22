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

#include "nvmain.h"
#include "src/Config.h"
#include "src/AddressTranslator.h"
#include "src/Interconnect.h"
#include "src/SimInterface.h"
#include "src/EventQueue.h"
#include "Interconnect/InterconnectFactory.h"
#include "MemControl/MemoryControllerFactory.h"
#include "traceWriter/TraceWriterFactory.h"
#include "Decoders/DecoderFactory.h"
#include "Utils/HookFactory.h"
#include "include/NVMainRequest.h"
#include "include/NVMHelpers.h"
#include "Prefetchers/PrefetcherFactory.h"

#include <sstream>
#include <cassert>

using namespace std;
using namespace NVM;

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

NVMain::NVMain( )
{
    config = NULL;
    translator = NULL;
    memoryControllers = NULL;
    channelConfig = NULL;
    syncValue = 0.0f;
    preTracer = NULL;

    totalReadRequests = 0;
    totalWriteRequests = 0;

    prefetcher = NULL;
    successfulPrefetches = 0;
    unsuccessfulPrefetches = 0;

    for(int i=0; i<8; i++) {
        pattern_statistic[i] = 0;
    }
}

NVMain::~NVMain( )
{
    if( config ) 
        delete config;
    
    if( memoryControllers )
    {
        for( unsigned int i = 0; i < numChannels; i++ )
        {
            if( memoryControllers[i] )
                delete memoryControllers[i];
        }

        delete [] memoryControllers;
    }

    if( translator )
        delete translator;

    if( channelConfig )
    {
        for( unsigned int i = 0; i < numChannels; i++ )
        {
            delete channelConfig[i];
        }

        delete [] channelConfig;
    }
}

Config *NVMain::GetConfig( )
{
    return config;
}

void NVMain::SetConfig( Config *conf, std::string memoryName, bool createChildren )
{
    TranslationMethod *method;
    int channels, ranks, banks, rows, cols, subarrays;

    Params *params = new Params( );
    params->SetParams( conf );
    SetParams( params );

    StatName( memoryName );

    config = conf;
    if( config->GetSimInterface( ) != NULL )
        config->GetSimInterface( )->SetConfig( conf, createChildren );
    else
      std::cout << "Warning: Sim Interface should be allocated before configuration!" << std::endl;


    if( createChildren )
    {
        if( conf->KeyExists( "MATHeight" ) )
        {
            rows = static_cast<int>(p->MATHeight);
            subarrays = static_cast<int>( p->ROWS / p->MATHeight );
        }
        else
        {
            rows = static_cast<int>(p->ROWS);
            subarrays = 1;
        }
        cols = static_cast<int>(p->COLS);
        banks = static_cast<int>(p->BANKS);
        ranks = static_cast<int>(p->RANKS);
        channels = static_cast<int>(p->CHANNELS);

        if( config->KeyExists( "Decoder" ) )
            translator = DecoderFactory::CreateNewDecoder( config->GetString( "Decoder" ) );
        else
            translator = new AddressTranslator( );

        method = new TranslationMethod( );

        method->SetBitWidths( NVM::mlog2( rows ), 
                    NVM::mlog2( cols ), 
                    NVM::mlog2( banks ), 
                    NVM::mlog2( ranks ), 
                    NVM::mlog2( channels ), 
                    NVM::mlog2( subarrays )
                    );
        method->SetCount( rows, cols, banks, ranks, channels, subarrays );
        method->SetAddressMappingScheme( p->AddressMappingScheme );
        translator->SetConfig( config, createChildren );
        translator->SetTranslationMethod( method );
        translator->SetDefaultField( CHANNEL_FIELD );

        SetDecoder( translator );

        memoryControllers = new MemoryController* [channels];
        channelConfig = new Config* [channels];
        for( int i = 0; i < channels; i++ )
        {
            std::stringstream confString;
            std::string channelConfigFile;

            channelConfig[i] = new Config( *config );

            channelConfig[i]->SetSimInterface( config->GetSimInterface( ) );

            confString << "CONFIG_CHANNEL" << i;

            if( config->GetString( confString.str( ) ) != "" )
            {
                channelConfigFile  = config->GetString( confString.str( ) );

                if( channelConfigFile[0] != '/' )
                {
                    channelConfigFile  = NVM::GetFilePath( config->GetFileName( ) );
                    channelConfigFile += config->GetString( confString.str( ) );
                }
                
                std::cout << "Reading channel config file: " << channelConfigFile << std::endl;

                channelConfig[i]->Read( channelConfigFile );
            }

            /* Initialize memory controller */
            memoryControllers[i] = 
                MemoryControllerFactory::CreateNewController( channelConfig[i]->GetString( "MEM_CTL" ) );

            /* When selecting a MC child, use no field from the decoder (only-child). */

            confString.str( "" );
            confString << memoryName << ".channel" << i << "." 
                << channelConfig[i]->GetString( "MEM_CTL" ); 
            memoryControllers[i]->StatName( confString.str( ) );
            memoryControllers[i]->SetID( i );

            AddChild( memoryControllers[i] );
            memoryControllers[i]->SetParent( this );

            /* Set Config recursively. */
            memoryControllers[i]->SetConfig( channelConfig[i], createChildren );

            /* Register statistics. */
            memoryControllers[i]->RegisterStats( );
        }

    }

    if( p->MemoryPrefetcher != "none" )
    {
        prefetcher = PrefetcherFactory::CreateNewPrefetcher( p->MemoryPrefetcher );
        std::cout << "Made a " << p->MemoryPrefetcher << " prefetcher." << std::endl;
    }

    numChannels = static_cast<unsigned int>(p->CHANNELS);
    
    std::string pretraceFile;

    if( p->PrintPreTrace || p->EchoPreTrace )
    {
        if( config->GetString( "PreTraceFile" ) == "" )
            pretraceFile = "trace.nvt";
        else
            pretraceFile = config->GetString( "PreTraceFile" );

        if( pretraceFile[0] != '/' )
        {
            pretraceFile  = NVM::GetFilePath( config->GetFileName( ) );
            pretraceFile += config->GetString( "PreTraceFile" );
        }

        std::cout << "Using trace file " << pretraceFile << std::endl;

        if( config->GetString( "PreTraceWriter" ) == "" )
            preTracer = TraceWriterFactory::CreateNewTraceWriter( "NVMainTrace" );
        else
            preTracer = TraceWriterFactory::CreateNewTraceWriter( config->GetString( "PreTraceWriter" ) );

        if( p->PrintPreTrace )
            preTracer->SetTraceFile( pretraceFile );
        if( p->EchoPreTrace )
            preTracer->SetEcho( true );
    }

    RegisterStats( );
}

bool NVMain::IsIssuable( NVMainRequest *request, FailReason *reason )
{
    uint64_t channel, rank, bank, row, col, subarray;
    bool rv;

    assert( request != NULL );

    GetDecoder( )->Translate( request->address.GetPhysicalAddress( ), 
                           &row, &col, &rank, &bank, &channel, &subarray );

    rv = memoryControllers[channel]->IsIssuable( request, reason );

    return rv;
}

void NVMain::GeneratePrefetches( NVMainRequest *request, std::vector<NVMAddress>& prefetchList )
{
    std::vector<NVMAddress>::iterator iter;
    ncounter_t channel, rank, bank, row, col, subarray;

    for( iter = prefetchList.begin(); iter != prefetchList.end(); iter++ )
    {
        /* Make a request from the prefetch address. */
        NVMainRequest *pfRequest = new NVMainRequest( );
        *pfRequest = *request;
        pfRequest->address = (*iter);
        pfRequest->isPrefetch = true;
        pfRequest->owner = this;
        
        /* Translate the address, then copy to the address struct, and copy to request. */
        GetDecoder( )->Translate( request->address.GetPhysicalAddress( ), 
                               &row, &col, &bank, &rank, &channel, &subarray );
        request->address.SetTranslatedAddress( row, col, bank, rank, channel, subarray );
        request->bulkCmd = CMD_NOP;

        //std::cout << "Prefetching 0x" << std::hex << (*iter).GetPhysicalAddress() << " (trigger 0x"
        //          << request->address.GetPhysicalAddress( ) << std::dec << std::endl;

        /* Just type to issue; If the queue is full it simply won't be enqueued. */
        GetChild( pfRequest )->IssueCommand( pfRequest );
    }
}

void NVMain::IssuePrefetch( NVMainRequest *request )
{
    /* 
     *  Generate prefetches here. It makes the most sense to prefetch in this class
     *  since we may prefetch across channels. However, this may be applicable to any
     *  class in the hierarchy as long as we filter out the prefetch addresses that
     *  do not belong to a child of the current module.
     */
    /* TODO: We are assuming this is the master root! */
    std::vector<NVMAddress> prefetchList;
    if( prefetcher && request->type == READ && request->isPrefetch == false 
        && prefetcher->DoPrefetch(request, prefetchList) )
    {
        GeneratePrefetches( request, prefetchList );
    }
}

bool NVMain::CheckPrefetch( NVMainRequest *request )
{
    bool rv = false;
    NVMainRequest *pfRequest = NULL;
    std::list<NVMainRequest *>::const_iterator iter;
    std::vector<NVMAddress> prefetchList;

    for( iter = prefetchBuffer.begin(); iter!= prefetchBuffer.end(); iter++ )
    {
        if( (*iter)->address.GetPhysicalAddress() == request->address.GetPhysicalAddress() )
        {
            if( prefetcher->NotifyAccess(request, prefetchList) )
            {
                GeneratePrefetches( request, prefetchList );
            }

            successfulPrefetches++;
            rv = true;
            pfRequest = (*iter);
            delete pfRequest;
            break;
        }
    }

    if( pfRequest != NULL )
    {
        //std::cout << "Prefetched 0x" << std::hex << request->address.GetPhysicalAddress( )
        //          << std::dec << " (list size " << prefetchBuffer.size() << " -> ";
        prefetchBuffer.remove( pfRequest );
        //std::cout << prefetchBuffer.size() << ")" << std::endl;
    }

    return rv;
}

void NVMain::PrintPreTrace( NVMainRequest *request )
{
    /*
     *  Here we can generate a data trace to use with trace-based testing later.
     */
    if( p->PrintPreTrace || p->EchoPreTrace )
    {
        TraceLine tl;

        tl.SetLine( request->address,
                    request->type,
                    GetEventQueue( )->GetCurrentCycle( ),
                    request->data,
                    request->oldData,
                    request->threadId 
                  );

        preTracer->SetNextAccess( &tl );
    }
}

bool NVMain::IssueCommand( NVMainRequest *request )
{
    ncounter_t channel, rank, bank, row, col, subarray;
    bool mc_rv;

    if( !config )
    {
        std::cout << "NVMain: Received request before configuration!\n";
        return false;
    }

    //request->data.SetSize(64);
    /* Translate the address, then copy to the address struct, and copy to request. */
    GetDecoder( )->Translate( request->address.GetPhysicalAddress( ), 
                           &row, &col, &bank, &rank, &channel, &subarray );
    request->address.SetTranslatedAddress( row, col, bank, rank, channel, subarray );
    request->bulkCmd = CMD_NOP;

    /* Check for any successful prefetches. */
    if( CheckPrefetch( request ) )
    {
        GetEventQueue()->InsertEvent( EventResponse, this, request, 
                                      GetEventQueue()->GetCurrentCycle() + 1 );

        return true;
    }

    assert( GetChild( request )->GetTrampoline( ) == memoryControllers[channel] );
    mc_rv = GetChild( request )->IssueCommand( request );
    if( mc_rv == true )
    {
        IssuePrefetch( request );

        if( request->type == READ ) 
        {
            totalReadRequests++;
        }
        else
        {
            totalWriteRequests++;
        }

        PrintPreTrace( request );
    }

    NVMDataBlock& newData = request->data;
    NVMDataBlock& oldData = request->oldData;
	
    // return 0;
    uint64_t byte_size = 0;
    uint64_t bit_off = 0;
    uint64_t old_data = 0;
    uint64_t new_data = 0;

    int64_t *ptr = (int64_t*)(newData.rawData);
    // newData.SetSize(64);
    // oldData.SetSize(64);
    //return 0;
    while(byte_size < newData.GetSize()) {
        old_data = *ptr;

        if(pattern_zero(old_data)) {
            new_data = 0;
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[0]++;
            bit_off += 3;
        } else if(pattern_one(old_data)) {
            new_data = old_data & 0xff;
            new_data |= (0x1 << 8);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[1]++;
            bit_off += 3 + 8;
        } else if(pattern_two(old_data)) {
            new_data = old_data & 0xffff;
            new_data |= (0x2 << 16);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[2]++;
            bit_off += 3 + 16;
        } else if(pattern_three(old_data)) {
            new_data = old_data & 0xffffffff;
            new_data |= ((uint64_t)0x3 << 32);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[3]++;
            bit_off += 3 + 32;
        } else if(pattern_four(old_data)) {
            new_data = old_data >> 32;
            new_data |= ((uint64_t)0x4 << 32);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[4]++;
            bit_off += 3 + 32;
        } else if(pattern_five(old_data)) {
            new_data = old_data & 0xffff;
            new_data |= ((old_data >> 32) & 0xffff) << 16;
            new_data |= ((uint64_t)0x5 << 32);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[5]++;
            bit_off += 3 + 32;
        } else if(pattern_six(old_data)) {
            new_data = old_data & 0xffff;
            new_data |= ((uint64_t)0x6 << 16);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[6]++;
            bit_off += 3 + 16;
        } else if(pattern_seven(old_data)) { // uncompressed
            new_data = old_data & 0xffffffff;
            //fill_data(oldData.rawData, bit_off, new_data);
            bit_off += 32;
            new_data = old_data >> 32;
            new_data |= ((uint64_t)0x7 << 32);
            //fill_data(oldData.rawData, bit_off, new_data);
            pattern_statistic[7]++;
            bit_off += 3 + 16;
        }

        // 下一个dword
        ptr++; 
        byte_size += 8;
    }

    return mc_rv;
}

bool NVMain::IssueAtomic( NVMainRequest *request )
{
    ncounter_t channel, rank, bank, row, col, subarray;
    bool mc_rv;

    if( !config )
    {
        std::cout << "NVMain: Received request before configuration!\n";
        return false;
    }

    /* Translate the address, then copy to the address struct, and copy to request. */
    GetDecoder( )->Translate( request->address.GetPhysicalAddress( ), 
                           &row, &col, &bank, &rank, &channel, &subarray );
    request->address.SetTranslatedAddress( row, col, bank, rank, channel, subarray );
    request->bulkCmd = CMD_NOP;

    /* Check for any successful prefetches. */
    if( CheckPrefetch( request ) )
    {
        return true;
    }

    mc_rv = memoryControllers[channel]->IssueAtomic( request );
    if( mc_rv == true )
    {
        IssuePrefetch( request );

        if( request->type == READ ) 
        {
            totalReadRequests++;
        }
        else
        {
            totalWriteRequests++;
        }

        PrintPreTrace( request );
    }
    
    return mc_rv;
}

bool NVMain::RequestComplete( NVMainRequest *request )
{
    bool rv = false;

    if( request->owner == this )
    {
        if( request->isPrefetch )
        {
            //std::cout << "Placing 0x" << std::hex << request->address.GetPhysicalAddress( )
            //          << std::dec << " into prefetch buffer (cur size: " << prefetchBuffer.size( )
            //          << ")." << std::endl;

            /* Place in prefetch buffer. */
            if( prefetchBuffer.size() >= p->PrefetchBufferSize )
            {
                unsuccessfulPrefetches++;
                //std::cout << "Prefetch buffer is full. Removing oldest prefetch: 0x" << std::hex
                //          << prefetchBuffer.front()->address.GetPhysicalAddress() << std::dec
                //          << std::endl;

                delete prefetchBuffer.front();
                prefetchBuffer.pop_front();
            }

            prefetchBuffer.push_back( request );
            rv = true;
        }
        else
        {
            delete request;
            rv = true;
        }
    }
    else
    {
        rv = GetParent( )->RequestComplete( request );
    }

    /* This is used in the main memory system when a DRAMCache is present.
     * DRAMCache misses need to issue main memory requests than might not
     * be issuable at that time. Try to issue these here. */
    if( !pendingMemoryRequests.empty() ){
       NVMainRequest *staleMemReq = pendingMemoryRequests.front();
        if( IsIssuable(staleMemReq, NULL) ) {
            IssueCommand( staleMemReq );
            pendingMemoryRequests.pop();
        }
    }

    return rv;
}

void NVMain::Cycle( ncycle_t /*steps*/ )
{
}

void NVMain::RegisterStats( )
{
    AddStat(totalReadRequests);
    AddStat(totalWriteRequests);
    AddStat(successfulPrefetches);
    AddStat(unsuccessfulPrefetches);

    AddStat(pattern_statistic[0]);
    AddStat(pattern_statistic[1]);
    AddStat(pattern_statistic[2]);
    AddStat(pattern_statistic[3]);
    AddStat(pattern_statistic[4]);
    AddStat(pattern_statistic[5]);
    AddStat(pattern_statistic[6]);
    AddStat(pattern_statistic[7]);    
}

void NVMain::CalculateStats( )
{
    for( unsigned int i = 0; i < numChannels; i++ )
        memoryControllers[i]->CalculateStats( );
}

void NVMain::EnqueuePendingMemoryRequests( NVMainRequest *req )
{
    pendingMemoryRequests.push(req);
}

