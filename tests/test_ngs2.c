/* NGS2 rendering and PS4 voice events. The decoder is deterministic so these checks need no
 * game files and can distinguish duplicated, skipped and out-of-order audio grains. */
#undef NDEBUG
#include "../src/runtime_ngs2.c"
#include <assert.h>
#include <math.h>

void *runtime_low_alloc(size_t size) { return calloc(1,size); }
void runtime_low_free(void *pointer) { free(pointer); }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* Each byte names four stereo samples in a four-frame superframe. Like LibAtrac9,
 * InitDecoder configures the codec without clearing a partially consumed superframe. */
typedef struct { unsigned frame; } TestDecoder;
void *Atrac9GetHandle(void) { return calloc(1,sizeof(TestDecoder)); }
void Atrac9ReleaseHandle(void *handle) { free(handle); }
int Atrac9InitDecoder(void *handle, unsigned char *config) {
    (void)config;
    return handle ? 0 : -1;
}
int Atrac9GetCodecInfo(void *handle, Atrac9CodecInfo *info) {
    if (!handle) return -1;
    *info=(Atrac9CodecInfo){.channels=2,.samplingRate=48000,.superframeSize=4,
                           .framesInSuperframe=4,.frameSamples=4};
    return 0;
}
int Atrac9DecodeF32(void *handle, const unsigned char *data, int size, float *pcm,
                    int *used, int nointerleave) {
    (void)nointerleave;
    if (!handle || size<1) return -1;
    TestDecoder *decoder=handle;
    if ((data[0]&3)!=decoder->frame) return -1;
    decoder->frame=(decoder->frame+1)&3;
    for (int i=0;i<4;++i)
        pcm[2*i]=pcm[2*i+1]=(float)(4*data[0]+i+1)/128.0f;
    *used=1;
    return 0;
}

typedef struct {
    void *system, *samplers, *masters, *submixers;
    void *source, *bus, *master[2];
    unsigned char data[64];
    float output[2][8];
    RenderBufferInfo buffers[2];
} Fixture;

static void event(void *voice, uint32_t value) {
    struct { VoiceParamHeader header; uint32_t value; } param={{12,0,6},value};
    assert(voice_control(voice,&param)==0);
}
static void patch(void *voice, uint32_t port, void *destination) {
    struct { VoiceParamHeader header; uint32_t port, input; void *destination; }
        param={{24,0,5},port,0,destination};
    assert(voice_control(voice,&param)==0);
}
static void volume(void *voice, uint32_t port, float level) {
    struct { VoiceParamHeader header; uint32_t port; float level; }
        param={{16,0,2},port,level};
    assert(voice_control(voice,&param)==0);
}
static void *make_rack(void *system, uint32_t id, uint32_t count) {
    unsigned char option[40]={0};
    uint64_t size=sizeof(option);
    memcpy(option,&size,sizeof(size));
    memcpy(option+32,&count,sizeof(count));
    void *rack=NULL;
    assert(rack_create(system,id,option,NULL,&rack)==0);
    return rack;
}
static void fixture_init(Fixture *f) {
    memset(f,0,sizeof(*f));
    assert(system_create(NULL,NULL,&f->system)==0);
    f->samplers=make_rack(f->system,0x4001,1);
    f->submixers=make_rack(f->system,0x2000,1);
    f->masters=make_rack(f->system,0x3000,2);
    assert(rack_voice_handle(f->samplers,0,&f->source)==0);
    assert(rack_voice_handle(f->submixers,0,&f->bus)==0);
    for (uint32_t i=0;i<2;++i) {
        assert(rack_voice_handle(f->masters,i,&f->master[i])==0);
        struct { VoiceParamHeader header; uint32_t output, reserved; }
            param={{16,0,0x30000005},i,0};
        assert(voice_control(f->master[i],&param)==0);
        event(f->master[i],0); /* PS4 PLAY is zero, not a bit mask. */
        f->buffers[i]=(RenderBufferInfo){f->output[i],sizeof(f->output[i]),0x18,2};
    }
    patch(f->source,0,f->bus);
    patch(f->bus,0,f->master[1]);
    event(f->bus,0);
    struct { VoiceParamHeader header; WaveformFormat format; uint32_t flags, reserved; }
        setup={{40,0,0x40010000},{0x40,2,48000,0xfe740bf0,0,0},0,0};
    assert(voice_control(f->source,&setup)==0);
    for (unsigned i=0;i<sizeof(f->data);++i) f->data[i]=(unsigned char)i;
}
static void fixture_destroy(Fixture *f) {
    assert(rack_destroy(f->samplers,NULL)==0);
    assert(rack_destroy(f->submixers,NULL)==0);
    assert(rack_destroy(f->masters,NULL)==0);
    assert(system_destroy(f->system,NULL)==0);
}
static void submit(Fixture *f, const WaveformBlock *block) {
    struct { VoiceParamHeader header; const void *data; uint32_t flags, count;
             const WaveformBlock *blocks; }
        param={{32,0,0x40010001},f->data,1,1,block};
    assert(voice_control(f->source,&param)==0);
}
static void waveform(Fixture *f) {
    WaveformBlock block={.data_size=sizeof(f->data),.samples=sizeof(f->data)*4};
    submit(f,&block);
    /* The caller can reuse its descriptor as soon as VoiceControl returns. */
    memset(&block,0,sizeof(block));
}
static void render(Fixture *f) {
    memset(f->output,0xcc,sizeof(f->output));
    assert(system_render(f->system,f->buffers,2)==0);
}
static void silent(const float *output) {
    for (unsigned i=0;i<8;++i) assert(output[i]==0.0f);
}
static void samples(const float *output, unsigned first, float gain) {
    for (unsigned i=0;i<4;++i) {
        float expected=(float)(first+i)/128.0f*gain*(32767.0f/32768.0f);
        assert(fabsf(output[2*i]-expected)<0.000001f);
        assert(fabsf(output[2*i+1]-expected)<0.000001f);
    }
}
static void sample_values(const float *output, const unsigned values[4]) {
    for (unsigned i=0;i<4;++i) {
        float expected=(float)values[i]/128.0f*(32767.0f/32768.0f);
        assert(fabsf(output[2*i]-expected)<0.000001f);
        assert(fabsf(output[2*i+1]-expected)<0.000001f);
    }
}

static void test_output_routing_and_clock(void) {
    Fixture f; fixture_init(&f);
    waveform(&f);
    render(&f);
    silent(f.output[0]); silent(f.output[1]); /* Submitting data is not PLAY. */
    event(f.source,0);
    render(&f);
    silent(f.output[0]); samples(f.output[1],1,1.0f);
    assert(system_render(f.system,NULL,0)==0); /* No output buffers must not end playback. */

    /* Two output destinations receive the same time slice. The second output must not consume
     * the next four samples, and the next render must continue exactly where this one ended. */
    patch(f.source,1,f.master[0]); volume(f.source,1,0.5f);
    render(&f);
    samples(f.output[0],5,0.5f); samples(f.output[1],5,1.0f);
    render(&f);
    samples(f.output[0],9,0.5f); samples(f.output[1],9,1.0f);

    event(f.source,4); /* PAUSE */
    render(&f); silent(f.output[0]); silent(f.output[1]);
    event(f.source,5); /* RESUME */
    render(&f);
    samples(f.output[0],13,0.5f); samples(f.output[1],13,1.0f);
    event(f.source,1); /* STOP */
    render(&f); silent(f.output[0]); silent(f.output[1]);
    fixture_destroy(&f);
}

static void test_play_before_waveform(void) {
    Fixture f; fixture_init(&f);
    event(f.source,0);
    waveform(&f);
    render(&f);
    silent(f.output[0]); samples(f.output[1],1,1.0f);
    event(f.source,3); /* KILL */
    render(&f); silent(f.output[0]); silent(f.output[1]);
    fixture_destroy(&f);
}

static void test_repeat_sample_windows(void) {
    Fixture f; fixture_init(&f);
    WaveformBlock block={.data_size=3,.repeats=1,.skip_samples=1,.samples=6};
    submit(&f,&block);
    /* The loop window and repeat count must survive reuse of Scream's temporary descriptors. */
    memset(&block,0,sizeof(block));
    event(f.source,0);
    const unsigned expected[3][4]={{2,3,4,5},{6,7,2,3},{4,5,6,7}};
    for (unsigned i=0;i<3;++i) {
        render(&f); silent(f.output[0]); sample_values(f.output[1],expected[i]);
    }
    render(&f); silent(f.output[0]); silent(f.output[1]);

    block=(WaveformBlock){.data_size=2,.repeats=UINT32_MAX,.skip_samples=1,.samples=3};
    submit(&f,&block);
    event(f.source,0);
    render(&f); sample_values(f.output[1],(const unsigned[4]){2,3,4,2});
    render(&f); sample_values(f.output[1],(const unsigned[4]){3,4,2,3});
    fixture_destroy(&f);
}

static void pcm_setup(Fixture *f, uint32_t type, uint32_t channels, uint32_t rate) {
    struct { VoiceParamHeader header; WaveformFormat format; uint32_t flags, reserved; }
        setup={{40,0,0x40010000},{type,channels,rate,0,0,0},0,0};
    assert(voice_control(f->source,&setup)==0);
}
static void pitch(void *voice, float ratio) {
    struct { VoiceParamHeader header; float ratio; uint32_t reserved; }
        param={{16,0,0x40010005},ratio,0};
    assert(voice_control(voice,&param)==0);
}
static void put_pcm(Fixture *f, uint32_t type, const float *values, unsigned count) {
    int short_sample=type==WAVEFORM_PCM_I16_LE || type==WAVEFORM_PCM_I16_BE;
    int big_endian=type==WAVEFORM_PCM_I16_BE || type==WAVEFORM_PCM_F32_BE;
    unsigned bytes=short_sample ? 2 : 4;
    assert(count*bytes<=sizeof(f->data));
    for (unsigned i=0;i<count;++i) {
        uint32_t bits;
        if (short_sample) bits=(uint16_t)(int16_t)(values[i]*32768.0f);
        else memcpy(&bits,&values[i],sizeof(bits));
        for (unsigned b=0;b<bytes;++b)
            f->data[i*bytes+b]=(unsigned char)(bits>>((big_endian ? bytes-1-b : b)*8));
    }
}
static void check_pcm(const float *output, const float *expected, unsigned count) {
    for (unsigned i=0;i<count;++i)
        assert(fabsf(output[i]-expected[i]*(32767.0f/32768.0f))<0.000002f);
}

static void test_pcm_formats_and_routing(void) {
    const uint32_t types[]={WAVEFORM_PCM_I16_LE,WAVEFORM_PCM_I16_BE,
                            WAVEFORM_PCM_F32_LE,WAVEFORM_PCM_F32_BE};
    const float stereo[]={0.125f,-0.25f,0.375f,-0.5f,0.625f,-0.75f,0.875f,-1.0f};
    for (unsigned t=0;t<4;++t) {
        Fixture f; fixture_init(&f);
        pcm_setup(&f,types[t],2,48000);
        put_pcm(&f,types[t],stereo,8);
        WaveformBlock block={.data_size=t<2 ? 16 : 32,.samples=4};
        submit(&f,&block); event(f.source,0);
        render(&f); silent(f.output[0]); check_pcm(f.output[1],stereo,8);
        render(&f); silent(f.output[1]);
        fixture_destroy(&f);
    }
    Fixture f; fixture_init(&f);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    const float mono[]={0.125f,0.25f,0.375f,0.5f};
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,4);
    WaveformBlock block={.data_size=8,.samples=4};
    submit(&f,&block); event(f.source,0);
    patch(f.source,1,f.master[0]); volume(f.source,1,0.5f);
    render(&f);
    for (unsigned i=0;i<4;++i) {
        float expected[]={mono[i],mono[i]};
        check_pcm(f.output[1]+2*i,expected,2);
        expected[0]*=0.5f; expected[1]*=0.5f;
        check_pcm(f.output[0]+2*i,expected,2);
    }
    fixture_destroy(&f);
}

static void test_pcm_windows_pause_and_partial_frames(void) {
    Fixture f; fixture_init(&f);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    const float mono[]={0.125f,0.25f,0.375f,0.5f,0.625f};
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,5);
    WaveformBlock block={.data_offset=2,.data_size=8,.skip_samples=1,.samples=2,.repeats=1};
    submit(&f,&block); event(f.source,0);
    event(f.source,4); render(&f); silent(f.output[1]);
    event(f.source,5); render(&f);
    const float expected[]={0.375f,0.375f,0.5f,0.5f,0.375f,0.375f,0.5f,0.5f};
    check_pcm(f.output[1],expected,8);
    render(&f); silent(f.output[1]);

    pcm_setup(&f,WAVEFORM_PCM_I16_LE,2,48000);
    block=(WaveformBlock){.data_size=3,.repeats=UINT32_MAX};
    submit(&f,&block); event(f.source,0);
    render(&f); silent(f.output[1]); /* No complete stereo frame, including an infinite loop. */
    uint32_t flags; assert(voice_get_state_flags(f.source,&flags)==0);
    assert(flags&VOICE_STATE_EMPTY);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    block=(WaveformBlock){.data_size=8,.skip_samples=4,.repeats=UINT32_MAX};
    submit(&f,&block); event(f.source,0);
    render(&f); silent(f.output[1]); /* An empty skip window cannot repeat forever either. */
    fixture_destroy(&f);
}

static void test_pcm_resampling_across_grains(void) {
    Fixture f; fixture_init(&f);
    /* Scream submits 44.1 kHz effects as 48 kHz PCM with a 44100/48000 pitch ratio. */
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    float mono[32];
    for (unsigned i=0;i<32;++i) mono[i]=(float)i/64.0f;
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,32);
    WaveformBlock block={.data_size=64,.samples=32};
    submit(&f,&block); pitch(f.source,0.91875f); event(f.source,0);
    for (unsigned grain=0;grain<4;++grain) {
        render(&f);
        for (unsigned i=0;i<4;++i) {
            float value=(float)((grain*4+i)*(double)0.91875f/64.0);
            float expected[]={value,value}; check_pcm(f.output[1]+2*i,expected,2);
        }
        if (grain==1) {
            event(f.source,4); render(&f); silent(f.output[1]); event(f.source,5);
        }
    }
    /* A non-default system clock uses the same conversion without a pitch parameter. */
    ((System *)f.system)->sample_rate=32000;
    event(f.source,1); pitch(f.source,1.0f); submit(&f,&block); event(f.source,0);
    render(&f);
    for (unsigned i=0;i<4;++i) {
        float value=(float)i*1.5f/64.0f;
        float expected[]={value,value}; check_pcm(f.output[1]+2*i,expected,2);
    }
    fixture_destroy(&f);
}

static void test_stopped_effects_return_to_pool(void) {
    Fixture f; fixture_init(&f);
    pcm_setup(&f,WAVEFORM_PCM_I16_LE,1,48000);
    const float mono[]={0.125f,0.25f,0.375f,0.5f};
    put_pcm(&f,WAVEFORM_PCM_I16_LE,mono,4);
    WaveformBlock block={.data_size=8,.samples=4,.repeats=UINT32_MAX};
    uint32_t flags;
    for (unsigned i=0;i<120;++i) {
        /* Scream's pool scans for state zero or EMPTY before starting another effect. */
        if (i) {
            assert(voice_get_state_flags(f.source,&flags)==0);
            assert(flags==0 || (flags&VOICE_STATE_EMPTY));
        }
        submit(&f,&block); event(f.source,0);
        render(&f); assert(f.output[1][0]>0.0f);
        event(f.source,1+i%3); /* STOP, STOP_IMMEDIATE, KILL */
        render(&f); silent(f.output[1]);
    }
    fixture_destroy(&f);
}

int main(void) {
    test_output_routing_and_clock();
    test_play_before_waveform();
    test_repeat_sample_windows();
    test_pcm_formats_and_routing();
    test_pcm_windows_pause_and_partial_frames();
    test_pcm_resampling_across_grains();
    test_stopped_effects_return_to_pool();
    puts("NGS2 routing, grain clock, PS4 events and PCM effects passed");
    return 0;
}
