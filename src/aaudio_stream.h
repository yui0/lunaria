/* MPL-2.0. AAudio's per-builder configuration and owned PCM stream buffer.
 * Android 12 contracts: frameworks/av/media/libaaudio/{include/aaudio/AAudio.h,
 * src/core/AudioStreamBuilder.cpp,src/core/AAudioStreamParameters.cpp}.
 * The desktop sink is shared PCM output; exclusive/MMAP/low-latency requests
 * are hints and never reported as negotiated capabilities of that sink. */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace luna_aaudio {
enum Result : int32_t { ok=0,disconnected=-899,illegal=-898,internal=-896,
    invalid_state=-895,invalid_handle=-892,unimplemented=-890,unavailable=-889,
    no_handles=-888,no_memory=-887,null_pointer=-886,timeout=-885,
    would_block=-884,invalid_format=-883,out_of_range=-882,no_service=-881,invalid_rate=-880 };
enum State : int32_t { open=2,starting=3,started=4,pausing=5,paused=6,
    flushing=7,flushed=8,stopping=9,stopped=10,closing=11,closed=12,dead=13 };
struct Config {
    int32_t direction=0,channels=0,format=0,sample_rate=0,callback_frames=0;
    int32_t capacity=0,sharing=1,performance=10,device=0;
    int32_t usage=1,content_type=2,input_preset=6;
    uint64_t data_cb=0,user_data=0,error_cb=0,error_user=0;
    int validate() const {
        if(direction!=0 && direction!=1)return illegal;
        if(sharing!=0 && sharing!=1)return illegal;
        if(performance<10 || performance>12)return illegal;
        if(usage!=0 && usage!=1 && usage!=2 && usage!=3 && usage!=4 && usage!=5 && usage!=6 && usage!=10 && usage!=11 && usage!=12 && usage!=13 && usage!=14 && usage!=16 && !(usage>=1000 && usage<=1003))return illegal;
        if(content_type<0 || content_type>4)return illegal;
        if(input_preset!=0 && input_preset!=1 && input_preset!=5 && input_preset!=6 && input_preset!=7 && input_preset!=9 && input_preset!=10)return illegal;

        if(channels<0 || channels>32 || capacity<0 || device<0 || callback_frames<0 || callback_frames>1024*1024)return out_of_range;
        if(format<0 || format>4)return invalid_format;
        if(sample_rate && (sample_rate<8000 || sample_rate>1600000))return invalid_rate;
        return ok;
    }
};
struct Stream : Config {
    State state=open;
    uint32_t burst=0,buffer_size=0,worker_tid=0,callback_args=0;
    uint64_t next_due_ns=0,presentation_end_ns=0,frames_written=0,frames_read=0;
    bool callback_pending=false,underflow=false,drain_on_stop=false;
    int32_t xruns=0;
    uint32_t head=0,queued=0,phase=0;
    std::vector<int16_t> pcm;
    explicit Stream(const Config &config=Config{}):Config(config){}
    int negotiate() {
        int error=validate();if(error)return error;
        /* There is no capture endpoint in the host audio backend yet. Do not
         * open a microphone stream that fabricates captured silence. */
        if(direction)return unavailable;
        if(device && device!=1)return unavailable;
        if(!channels)channels=2;
        if(!format)format=2;
        if(!sample_rate)sample_rate=48000;
        device=1;sharing=1;performance=10;
        burst=(uint32_t)std::max(1,sample_rate/100); // the shared sink's 10 ms quantum
        if(!callback_frames)callback_frames=(int32_t)burst;
        uint64_t requested=capacity ? (uint32_t)capacity : (uint64_t)burst*8;
        requested=std::max(requested,(uint64_t)(uint32_t)callback_frames*2);
        requested=std::max(requested,(uint64_t)burst*2);
        if(requested>INT32_MAX || requested*(uint32_t)channels>SIZE_MAX/sizeof(int16_t))return out_of_range;
        capacity=(int32_t)requested;buffer_size=(uint32_t)capacity;
        try {pcm.resize((size_t)capacity*(uint32_t)channels);}catch(...){return no_memory;}
        return ok;
    }
    bool playing() const {return state==started || (state==stopping && drain_on_stop);}
    void update_state(uint64_t now) {
        if(state==stopping && !worker_tid && (!drain_on_stop || (!queued && now>=presentation_end_ns)))state=stopped;
    }
    unsigned sample_bytes() const {return format==1 ? 2u : format==3 ? 3u : 4u;}
    uint32_t available() const {return queued>=buffer_size ? 0 : buffer_size-queued;}
    int resize(int32_t requested) {
        if(requested<0)return out_of_range;
        int32_t minimum=std::max<int32_t>((int32_t)burst,data_cb ? callback_frames : 0);
        buffer_size=(uint32_t)std::min<int32_t>(capacity,std::max<int32_t>(minimum,requested));
        return (int)buffer_size;
    }
    void clear() {frames_read+=queued;head=queued=phase=0;}
    int enqueue(const void *data,uint32_t frames) {
        frames=std::min(frames,available());if(!frames)return 0;
        if(!data)return null_pointer;
        const uint8_t *bytes=(const uint8_t *)data;unsigned width=sample_bytes();
        for(uint32_t i=0;i<frames;++i)for(int32_t c=0;c<channels;++c){
            size_t index=(size_t)i*(uint32_t)channels+(uint32_t)c;
            const uint8_t *p=bytes+index*width;int32_t value=0;
            if(format==1){int16_t sample;std::memcpy(&sample,p,2);value=sample;}
            else if(format==2){float sample;std::memcpy(&sample,p,4);
                value=std::isnan(sample) ? 0 : sample<=-1 ? -32768 : sample>=1 ? 32767 : std::min<int32_t>(32767,(int32_t)std::lrint(sample*32768.0f));}
            else if(format==3){value=(int32_t)((uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16);if(value&0x800000)value|=(int32_t)0xff000000;value>>=8;}
            else{std::memcpy(&value,p,4);value>>=16;}
            pcm[(((size_t)head+queued+i)%(uint32_t)capacity)*(uint32_t)channels+(uint32_t)c]=(int16_t)value;
        }
        queued+=frames;frames_written+=frames;return (int)frames;
    }
    unsigned mix(int32_t *output,unsigned frames,unsigned output_rate,unsigned output_channels,bool advance) {
        if(!output_rate || !output_channels || !queued)return 0;
        uint32_t pos=head,left=queued,remainder=phase;unsigned produced=0;uint64_t consumed=0;
        uint32_t owed=std::min(left,remainder/output_rate);
        remainder-=owed*output_rate;pos=(pos+owed)%(uint32_t)capacity;left-=owed;consumed+=owed;
        while(produced<frames && left){
            if(output)for(unsigned c=0;c<output_channels;++c){
                int32_t sample=0;
                if(output_channels==1 && channels>1){for(int32_t j=0;j<channels;++j)sample+=pcm[(size_t)pos*(uint32_t)channels+(uint32_t)j];sample/=channels;}
                else sample=pcm[(size_t)pos*(uint32_t)channels+(channels==1 ? 0 : c%(uint32_t)channels)];
                output[(size_t)produced*output_channels+c]+=sample;
            }
            ++produced;remainder+=(uint32_t)sample_rate;
            uint32_t n=std::min(left,remainder/output_rate);remainder-=n*output_rate;
            pos=(pos+n)%(uint32_t)capacity;left-=n;consumed+=n;
        }
        if(advance){head=pos;queued=left;phase=remainder;frames_read+=consumed;}
        return produced;
    }
};
}
