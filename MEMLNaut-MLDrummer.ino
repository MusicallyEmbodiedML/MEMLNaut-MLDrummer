// TODO:
/*
- Remove striped RAM with pico_set_binary_type( TARGET blocked_ram )
OR
- Put audio buffers and classes in unstriped ram (SRAM8, SRAM9)
*/

// #include "src/memllib/interface/InterfaceBase.hpp"
#include "src/memllib/interface/MIDIInOut.hpp"
//#include "src/memllib/hardware/memlnaut/display.hpp"
#include "src/memllib/audio/AudioAppBase.hpp"
#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/hardware/memlnaut/MEMLNaut.hpp"
#include <memory>
#include <new> // for placement new

#define XIASRI 1
#define USE_POPR    0

#if USE_POPR
#include "src/memllib/examples/IMLInterface.hpp"
#define INTERFACE_TYPE IMLInterface
#else
#include "src/memllib/examples/InterfaceRL.hpp"
#define INTERFACE_TYPE InterfaceRL
#endif
#include "src/memllib/synth/maxiPAF.hpp"
#include "hardware/structs/bus_ctrl.h"
#include "src/memllib/utils/sharedMem.hpp"
//#include "src/memllib/examples/MLDrummer.hpp"
#include "src/memllib/examples/KassiaAudioApp.hpp"
#include "src/memllib/synth/SaxAnalysis.hpp"
#include "src/memlp/Utils.h"
#include "src/memllib/hardware/memlnaut/Pins.hpp"
#include "src/memllib/utils/Maths.hpp"


#define APP_SRAM __not_in_flash("app")

static constexpr char APP_NAME[] = "-- MLDrummer Betty --";

// Statically allocated, properly aligned storage in AUDIO_MEM for objects
alignas(KassiaAudioApp) char AUDIO_MEM audio_app_mem[sizeof(KassiaAudioApp)];
alignas(SaxAnalysis)  char AUDIO_MEM saxAnalysis_mem[sizeof(SaxAnalysis)];

//display APP_SRAM scr;

bool core1_disable_systick = true;
bool core1_separate_stack = true;


uint32_t get_rosc_entropy_seed(int bits) {
    uint32_t seed = 0;
    for (int i = 0; i < bits; ++i) {
        // Wait for a bit of time to allow jitter to accumulate
        busy_wait_us_32(5);
        // Pull LSB from ROSC rand output
        seed <<= 1;
        seed |= (rosc_hw->randombit & 1);
    }
    return seed;
}


// Global objects
std::shared_ptr<INTERFACE_TYPE> APP_SRAM interface;

std::shared_ptr<MIDIInOut> midi_interf;
//std::shared_ptr<display> scr_ptr;

std::shared_ptr<KassiaAudioApp> AUDIO_MEM audio_app;
// Initialize with nullptr and a dummy deleter that can be default-constructed
std::unique_ptr<SaxAnalysis, void(*)(SaxAnalysis*)> AUDIO_MEM saxAnalysis{nullptr, [](SaxAnalysis*){}};
SharedBuffer<float, SaxAnalysis::kN_Params> machine_list_buffer;

// Inter-core communication
volatile bool APP_SRAM core_0_ready = false;
volatile bool APP_SRAM core_1_ready = false;
volatile bool APP_SRAM serial_ready = false;
volatile bool APP_SRAM interface_ready = false;


// Bind inputs to SaxAnalysis machine listening
constexpr size_t kN_InputParams = SaxAnalysis::kN_Params;


void setup()
{

    // FILE *fp = fopen("/thisfilelivesonflash.txt", "w");
    // fprintf(fp, "Hello!\n");
    // fclose(fp);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    uint32_t seed = get_rosc_entropy_seed(32);
    srand(seed);

    Serial.begin(115200);
    //while (!Serial) {}
    Serial.println("Serial initialised.");
    WRITE_VOLATILE(serial_ready, true);

    // Run tests if needed
    if (Tests::testMedianAbsoluteDeviation()) {
        Serial.println("___ MAD tests passed. ___");
    } else {
        Serial.println("___ MAD TESTS FAILED! ___");
    }
    if (Tests::testMeanAbsoluteDeviation()) {
        Serial.println("___ MeanAD tests passed. ___");
    } else {
        Serial.println("___ MeanAD TESTS FAILED! ___");
    }

    // Setup board
    MEMLNaut::Initialize();
    pinMode(33, OUTPUT);
    {
        auto temp_interface = std::make_shared<INTERFACE_TYPE>();
        temp_interface->setup(kN_InputParams, KassiaAudioApp::kN_Params);
        MEMORY_BARRIER();
        interface = temp_interface;
        MEMORY_BARRIER();
    }
    // Setup interface with memory barrier protection
    WRITE_VOLATILE(interface_ready, true);
    // Bind interface after ensuring it's fully initialized
    interface->bindInterface(true);
    Serial.println("Bound interface to MEMLNaut.");

    midi_interf = std::make_shared<MIDIInOut>();
    midi_interf->Setup(4);
    midi_interf->SetMIDISendChannel(1);
    Serial.println("MIDI setup complete.");

    // Bind MIDI
    interface->bindMIDI(midi_interf);

    WRITE_VOLATILE(core_0_ready, true);
    while (!READ_VOLATILE(core_1_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    //scr_ptr->post(APP_NAME);
    //add_repeating_timer_ms(-39, displayUpdate, NULL, &timerDisplay);
    std::shared_ptr<MessageView> helpView = std::make_shared<MessageView>("Help");
    helpView->post(APP_NAME);
    helpView->post("TA: Down: Forget replay memory");
    helpView->post("MA: Up: Randomise actor");
    helpView->post("MA: Down: Randomise critic");
    helpView->post("MB: Up: Positive reward");
    helpView->post("MB: Down: Negative reward");
    helpView->post("Y: Optimisation rate");
    helpView->post("Z: OU noise");
    helpView->post("Joystick: Explore");
    MEMLNaut::Instance()->disp->AddView(helpView);

    MEMLNaut::Instance()->addSystemInfoView();

    Serial.println("Finished initialising core 0.");
}

void loop()
{
    static size_t last_time_1s = 0;
    static size_t last_time_20ms = 0;
    static size_t last_time_1ms = 0;
    size_t current_time_ms = millis();

    // tasks to run every 1ms
    if (current_time_ms - last_time_1ms >= 1) {
        last_time_1ms = current_time_ms;
        // Poll MIDI interface
        if (midi_interf) {
            midi_interf->Poll();
        }
    }

    // tasks to run every 20ms
    if (current_time_ms - last_time_20ms >= 20) {
        last_time_20ms = current_time_ms;

#if 1
        // Read SharedBuffer
        std::vector<float> mlist_params(SaxAnalysis::kN_Params, 0);
        machine_list_buffer.readNonBlocking(mlist_params);
        // Send parameters to RL interface
        interface->readAnalysisParameters(mlist_params);

        // Read pots and run inference loop
        MEMLNaut::Instance()->loop();
#endif

        // Un-blink LED
        digitalWrite(Pins::LED, LOW);
    }

    // Tasks to run every 1000ms
    if (current_time_ms - last_time_1s >= 1000) {
        last_time_1s = current_time_ms;

        Serial.println(".");
        // Blink LED
        digitalWrite(Pins::LED, HIGH);
    }
}

stereosample_t AUDIO_FUNC(audio_callback)(stereosample_t x)
{
    digitalWrite(Pins::LED_TIMING, HIGH);

    stereosample_t y;
    // Audio processing
    if (audio_app) {
        y = audio_app->Process(x);
    } else {
        y = x; // Pass through if audio_app is not ready
    }

    // Machine listening
#if 1
    union {
        SaxAnalysis::parameters_t p;
        float v[SaxAnalysis::kN_Params];
    } param_u;
    param_u.p = saxAnalysis->Process(x.L + x.R);
    //WRITE_VOLATILE_STRUCT(sharedMem::saxParams, params);
    // Write params into shared_buffer
    machine_list_buffer.writeNonBlocking(param_u.v, SaxAnalysis::kN_Params);
#endif

    digitalWrite(Pins::LED_TIMING, LOW);

    return y;
}

void AUDIO_FUNC(audio_block_callback)(float in[][kBufferSize], float out[][kBufferSize], size_t n_channels, size_t n_frames)
{
    digitalWrite(Pins::LED_TIMING, HIGH);
    for (size_t i = 0; i < n_frames; ++i) {

        stereosample_t x {
            in[0][i],
            in[1][i]
        }, y;

        // Audio processing
        if (audio_app) {
            y = audio_app->Process(x);
        } else {
            y = x; // Pass through if audio_app is not ready
            y.L *= y.L;
            y.R *= y.R;
        }

        // Machine listening
    #if 1
        union {
            SaxAnalysis::parameters_t p;
            float v[SaxAnalysis::kN_Params];
        } param_u;

        //digitalWrite(Pins::LED_TIMING, HIGH);
        param_u.p = saxAnalysis->Process(x.L + x.R);
        //digitalWrite(Pins::LED_TIMING, LOW);

        //WRITE_VOLATILE_STRUCT(sharedMem::saxParams, params);
        // Write params into shared_buffer
        machine_list_buffer.writeNonBlocking(param_u.v, SaxAnalysis::kN_Params);
    #endif

        out[0][i] = y.L;
        out[1][i] = y.R;
    }
    digitalWrite(Pins::LED_TIMING, LOW);

}

void setup1()
{
    while (!READ_VOLATILE(serial_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    while (!READ_VOLATILE(interface_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    // Construct SaxAnalysis in statically allocated buffer using placement-new
    {
        SaxAnalysis* sax_raw = new (saxAnalysis_mem) SaxAnalysis(AudioDriver::GetSampleRate());
        // function-pointer deleter that only calls destructor (no free)
        void (*sax_fp_deleter)(SaxAnalysis*) = [](SaxAnalysis* p) { if (p) p->~SaxAnalysis(); };
        saxAnalysis = std::unique_ptr<SaxAnalysis, void(*)(SaxAnalysis*)>(sax_raw, sax_fp_deleter);
    }

    // Create audio app using placement-new into static buffer and custom deleter
    {
        KassiaAudioApp* audio_raw = new (audio_app_mem) KassiaAudioApp();
        std::shared_ptr<InterfaceBase> selectedInterface = std::dynamic_pointer_cast<InterfaceBase>(interface);

        audio_raw->Setup(AudioDriver::GetSampleRate(), selectedInterface);

        // shared_ptr with custom deleter calling only the destructor (control block still allocates)
        auto audio_deleter = [](KassiaAudioApp* p) { if (p) p->~KassiaAudioApp(); };
        std::shared_ptr<KassiaAudioApp> temp_audio_app(audio_raw, audio_deleter);

        MEMORY_BARRIER();
        audio_app = temp_audio_app;
        MEMORY_BARRIER();
    }

    // Override audio callback
    //AudioDriver::SetCallback(audio_callback);
    AudioDriver::SetBlockCallback(audio_block_callback);

    // Start audio driver
    AudioDriver::Setup();

    WRITE_VOLATILE(core_1_ready, true);
    while (!READ_VOLATILE(core_0_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    Serial.println("Finished initialising core 1.");
}

void loop1()
{
    // Audio app parameter processing loop
    audio_app->loop();
    delay(1);
}

