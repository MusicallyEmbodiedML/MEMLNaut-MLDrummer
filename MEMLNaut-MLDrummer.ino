// #include "src/memllib/interface/InterfaceBase.hpp"
#include "src/memllib/interface/MIDIInOut.hpp"
#include "src/memllib/hardware/memlnaut/display.hpp"
#include "src/memllib/audio/AudioAppBase.hpp"
#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/hardware/memlnaut/MEMLNaut.hpp"
#include <memory>

#define XIASRI 1
#define USE_POPR    1

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
#include "src/memllib/examples/MLDrummer.hpp"
#include "src/memllib/synth/SaxAnalysis.hpp"
#include "src/memlp/Utils.h"


#define APP_SRAM __not_in_flash("app")

static constexpr char APP_NAME[] = "-- MLDrummer Betty --";

display APP_SRAM scr;

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
std::shared_ptr<display> scr_ptr;

std::shared_ptr<MLDrummer> __scratch_y("audio") audio_app;
std::unique_ptr<SaxAnalysis> saxAnalysis;
SharedBuffer<float, SaxAnalysis::kN_Params> machine_list_buffer;

// Inter-core communication
volatile bool APP_SRAM core_0_ready = false;
volatile bool APP_SRAM core_1_ready = false;
volatile bool APP_SRAM serial_ready = false;
volatile bool APP_SRAM interface_ready = false;


// Bind inputs to SaxAnalysis machine listening
constexpr size_t kN_InputParams = SaxAnalysis::kN_Params;


struct repeating_timer APP_SRAM timerDisplay;
inline bool __not_in_flash_func(displayUpdate)(__unused struct repeating_timer *t) {
    scr_ptr->update();
    return true;
}

void setup()
{

    // FILE *fp = fopen("/thisfilelivesonflash.txt", "w");
    // fprintf(fp, "Hello!\n");
    // fclose(fp);

    scr_ptr = std::make_shared<display>();
    scr_ptr->setup();
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    uint32_t seed = get_rosc_entropy_seed(32);
    srand(seed);

    Serial.begin(115200);
    //while (!Serial) {}
    Serial.println("Serial initialised.");
    WRITE_VOLATILE(serial_ready, true);

    // Setup board
    MEMLNaut::Initialize();
    pinMode(33, OUTPUT);
    {
        auto temp_interface = std::make_shared<INTERFACE_TYPE>();
        temp_interface->setup(kN_InputParams, MLDrummer::kN_Params, scr_ptr);
        MEMORY_BARRIER();
        interface = temp_interface;
        MEMORY_BARRIER();
    }
    // Setup interface with memory barrier protection
    WRITE_VOLATILE(interface_ready, true);
    // Bind interface after ensuring it's fully initialized
    interface->bindInterface(true);
    Serial.println("Bound RL interface to MEMLNaut.");

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

    scr_ptr->post(APP_NAME);
    add_repeating_timer_ms(-39, displayUpdate, NULL, &timerDisplay);

    Serial.println("Finished initialising core 0.");
}

void loop()
{
    static size_t last_time_1s = 0;
    static size_t last_time_20ms = 0;
    static size_t last_time_1ms = 0;
    size_t current_time_ms = millis();

    // Tasks to run every 1000ms
    if (current_time_ms - last_time_1s >= 1000) {
        last_time_1s = current_time_ms;

        Serial.println(".");
        // Blink LED
        digitalWrite(33, HIGH);
    } else {
        // Un-blink LED
        digitalWrite(33, LOW);
    }

    // tasks to run every 20ms
    if (current_time_ms - last_time_20ms >= 20) {
        last_time_20ms = current_time_ms;

        // Read SharedBuffer
        std::vector<float> mlist_params(SaxAnalysis::kN_Params, 0);
        machine_list_buffer.readNonBlocking(mlist_params);
        for (unsigned int n = 0; n < SaxAnalysis::kN_Params; ++n) {
            if (scr_ptr) {
                scr_ptr->statusPost(String(mlist_params[n], 4), n);
            }
        }
        // Send parameters to RL interface
        interface->readAnalysisParameters(mlist_params);

        // Read pots and run inference loop
        MEMLNaut::Instance()->loop();
    }

    // tasks to run every 1ms
    if (current_time_ms - last_time_1ms >= 1) {
        last_time_1ms = current_time_ms;
        // Poll MIDI interface
        if (midi_interf) {
            midi_interf->Poll();
        }
    }
}

stereosample_t AUDIO_FUNC(audio_callback)(stereosample_t x)
{
    stereosample_t y;
    // Audio processing
    if (audio_app) {
        y = audio_app->Process(x);
    } else {
        y = x; // Pass through if audio_app is not ready
    }

    // Machine listening
    union {
        SaxAnalysis::parameters_t p;
        float v[SaxAnalysis::kN_Params];
    } param_u;
    param_u.p = saxAnalysis->Process(x.L + x.R);
    //WRITE_VOLATILE_STRUCT(sharedMem::saxParams, params);
    // Write params into shared_buffer
    machine_list_buffer.writeNonBlocking(param_u.v, SaxAnalysis::kN_Params);

    static size_t counter = 0;
    counter++;
    if (counter >= AudioDriver::GetSampleRate()) {
        counter = 0;
        Serial.println("+");
    }

    return y;
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

    saxAnalysis = std::make_unique<SaxAnalysis>(AudioDriver::GetSampleRate());

    // Create audio app with memory barrier protection
    {
        auto temp_audio_app = std::make_shared<MLDrummer>();
        std::shared_ptr<InterfaceBase> selectedInterface;

        selectedInterface = std::dynamic_pointer_cast<InterfaceBase>(interface);

        temp_audio_app->Setup(AudioDriver::GetSampleRate(), selectedInterface);
        MEMORY_BARRIER();
        audio_app = temp_audio_app;
        MEMORY_BARRIER();
    }

    // Override audio callback
    AudioDriver::SetCallback(audio_callback);

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

