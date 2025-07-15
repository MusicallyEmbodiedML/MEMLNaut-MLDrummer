// #include "src/memllib/interface/InterfaceBase.hpp"
#include "src/memllib/interface/MIDIInOut.hpp"
#include "src/memllib/hardware/memlnaut/display.hpp"
#include "src/memllib/audio/AudioAppBase.hpp"
#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/hardware/memlnaut/MEMLNaut.hpp"
#include <memory>
#define XIASRI 1
#include "src/memllib/examples/InterfaceRL.hpp"
#include "src/memllib/synth/maxiPAF.hpp"
#include "hardware/structs/bus_ctrl.h"
#include "sharedMem.hpp"
#include "src/memllib/examples/XiasriAudioApp.hpp"


#define APP_SRAM __not_in_flash("app")

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
std::shared_ptr<InterfaceRL> APP_SRAM RLInterface;

std::shared_ptr<MIDIInOut> midi_interf;


std::shared_ptr<PAFSynthApp> __scratch_y("audio") audio_app;

// Inter-core communication
volatile bool APP_SRAM core_0_ready = false;
volatile bool APP_SRAM core_1_ready = false;
volatile bool APP_SRAM serial_ready = false;
volatile bool APP_SRAM interface_ready = false;


// We're only bound to the joystick inputs (x, y, rotate)
constexpr size_t kN_InputParams = 0;


struct repeating_timer APP_SRAM timerDisplay;
inline bool __not_in_flash_func(displayUpdate)(__unused struct repeating_timer *t) {
    scr.update();
    return true;
}

void setup()
{

    // FILE *fp = fopen("/thisfilelivesonflash.txt", "w");
    // fprintf(fp, "Hello!\n");
    // fclose(fp);

    scr.setup();
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    uint32_t seed = get_rosc_entropy_seed(32);
    srand(seed);

    Serial.begin(115200);
    while (!Serial) {}
    Serial.println("Serial initialised.");
    WRITE_VOLATILE(serial_ready, true);

    // Setup board
    MEMLNaut::Initialize();
    pinMode(33, OUTPUT);
    {
        auto temp_interface = std::make_shared<interfaceRL>();
        temp_interface->setup(kN_InputParams, PAFSynthApp::kN_Params);
        MEMORY_BARRIER();
        RLInterface = temp_interface;
        MEMORY_BARRIER();
    }
    // Setup interface with memory barrier protection
    WRITE_VOLATILE(interface_ready, true);
    // Bind interface after ensuring it's fully initialized
    RLInterface->bindInterface(true);
    Serial.println("Bound RL interface to MEMLNaut.");

    midi_interf = std::make_shared<MIDIInOut>();
    midi_interf->Setup(4);
    midi_interf->SetMIDISendChannel(1);
    Serial.println("MIDI setup complete.");

    // Bind MIDI
    RLInterface->bindMIDI(midi_interf);

    WRITE_VOLATILE(core_0_ready, true);
    while (!READ_VOLATILE(core_1_ready)) {
        MEMORY_BARRIER();
        delay(1);
    }

    scr.post("MEMLNaut: let's go!");
    add_repeating_timer_ms(-39, displayUpdate, NULL, &timerDisplay);

    Serial.println("Finished initialising core 0.");
}

void loop()
{


    MEMLNaut::Instance()->loop();
    if (midi_interf) {
        midi_interf->Poll();
    }
    static int AUDIO_MEM blip_counter = 0;
    if (blip_counter++ > 30) {
        blip_counter = 0;
        Serial.println(".");
        // Blink LED
        digitalWrite(33, HIGH);
    } else {
        // Un-blink LED
        digitalWrite(33, LOW);
    }
    RLInterface->readAnalysisParameters();
    delay(20); // Add a small delay to avoid flooding the serial output
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


    // Create audio app with memory barrier protection
    {
        auto temp_audio_app = std::make_shared<PAFSynthApp>();
        std::shared_ptr<InterfaceBase> selectedInterface;

        if (mlMode == IML) {
            selectedInterface = std::dynamic_pointer_cast<InterfaceBase>(interfaceIML);
        } else {
            selectedInterface = std::dynamic_pointer_cast<InterfaceBase>(RLInterface);
        }

        temp_audio_app->Setup(AudioDriver::GetSampleRate(), selectedInterface);
        // temp_audio_app->Setup(AudioDriver::GetSampleRate(), dynamic_cast<std::shared_ptr<InterfaceBase>> (mlMode == IML ? interfaceIML : RLInterface));
        MEMORY_BARRIER();
        audio_app = temp_audio_app;
        MEMORY_BARRIER();
    }

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

extern "C" int getentropy (void * buffer, size_t how_many) {
    uint8_t* pBuf = (uint8_t*) buffer;
    while(how_many--) {
        uint8_t rand_val = rp2040.hwrand32() % UINT8_MAX;
        *pBuf++ = rand_val;
    }
    return 0; // return "no error". Can also do EFAULT, EIO, ENOSYS
}
