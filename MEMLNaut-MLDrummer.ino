// #include "src/memllib/interface/InterfaceBase.hpp"
#include "src/memllib/utils/MIDIInOut.hpp"
#include "display.hpp"
#include "src/memllib/audio/AudioAppBase.hpp"
#include "src/memllib/audio/AudioDriver.hpp"
#include "src/memllib/hardware/memlnaut/MEMLNaut.hpp"
#include <memory>
#include "IMLInterface.hpp"
#include "interfaceRL.hpp"
#include "src/memllib/synth/maxiPAF.hpp"
#include "hardware/structs/bus_ctrl.h"
#include "sharedMem.hpp"
#include "src/memllib/synth/OnePoleSmoother.hpp"
#include <VFS.h>
#include <LittleFS.h>

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

class PAFSynthApp : public AudioAppBase
{
public:
    static constexpr size_t kN_Params = 12;

    PAFSynthApp() : AudioAppBase(), smoother(150.f, kSampleRate),
        neuralNetOutputs(kN_Params, 0),
        smoothParams(kN_Params, 0)
     {}

    


    stereosample_t __force_inline Process(const stereosample_t x) override
    {
        float mix = x.L + x.R;
        float bpf1Val = bpf1.play(mix) * 100.f;
        bpf1Val = bpfEnv1.play(bpf1Val);
        WRITE_VOLATILE(sharedMem::f0, bpf1Val);

        float bpf2Val = bpf2.play(mix) * 100.f;
        bpf2Val = bpfEnv2.play(bpf2Val);
        WRITE_VOLATILE(sharedMem::f1, bpf2Val);
        
        float bpf3Val = bpf3.play(mix) * 100.f;
        bpf3Val = bpfEnv3.play(bpf3Val);
        WRITE_VOLATILE(sharedMem::f2, bpf3Val);
        
        float bpf4Val = bpf4.play(mix) * 100.f;
        bpf4Val = bpfEnv4.play(bpf4Val);
        WRITE_VOLATILE(sharedMem::f3, bpf4Val);

        smoother.Process(neuralNetOutputs.data(), smoothParams.data());


        dl1mix = smoothParams[0] * smoothParams[0] * 0.4f;
        dl2mix = smoothParams[1] * smoothParams[1] * 0.4f;
        dl3mix = smoothParams[2] * smoothParams[2] * 0.8f;
        allp1fb = smoothParams[4] * 0.99f;        
        allp2fb = smoothParams[5] * 0.99f;     
        float comb1fb = (smoothParams[6] * 0.95f);
        float comb2fb = (smoothParams[7] * 0.95f);

        float dl1fb = (smoothParams[8] * 0.95f);
        float dl2fb = (smoothParams[9] * 0.95f);
        float dl3fb = (smoothParams[10] * 0.95f);


        float y = dcb.play(mix, 0.99f) * 3.f;
        float y1 = allp1.allpass(y, 30, allp1fb);
        y1 = comb1.combfb(y1, 127, comb1fb);

        float y2 = allp2.allpass(y, 500, allp2fb);
        y2 = comb2.combfb(y2, 808, comb2fb);

        y = y1 + y2;
        float d1 = (dl1.play(y, 3500, dl1fb) * dl1mix);
        float d2 = (dl2.play(y, 9000, dl2fb) * dl2mix);
        float d3 = (dl3.play(y, 1199, dl3fb) * dl3mix);


        y = y + d1 + d2 + d3;
        
        y = tanhf(y);
        
        stereosample_t ret { y, y };
        
        frame++;



        return ret;
    }

    void Setup(float sample_rate, std::shared_ptr<InterfaceBase> interface) override
    {
        AudioAppBase::Setup(sample_rate, interface);
        maxiSettings::sampleRate = sample_rate;
        bpf1.set(maxiBiquad::filterTypes::BANDPASS, 100.f, 5.f, 0.f);
        bpf2.set(maxiBiquad::filterTypes::BANDPASS, 500.f, 5.f, 0.f);
        bpf3.set(maxiBiquad::filterTypes::BANDPASS, 1000.f, 5.f, 0.f);
        bpf4.set(maxiBiquad::filterTypes::BANDPASS, 4000.f, 5.f, 0.f);
    }

    void ProcessParams(const std::vector<float>& params) override
    {
        neuralNetOutputs = params;   
        
    }

protected:

    maxiDelayline<5000> dl1;
    maxiDelayline<15100> dl2;
    maxiDelayline<1201> dl3;

    maxiReverbFilters<300> allp1;
    maxiReverbFilters<1000> allp2;
    maxiReverbFilters<300> comb1;
    maxiReverbFilters<1000> comb2;

    std::vector<float> neuralNetOutputs, smoothParams;
    

    float frame=0;
    float dl1mix = 0.0f;
    float dl2mix = 0.0f;
    float dl3mix = 0.0f;
    float allp1fb=0.5f;
    float allp2fb=0.5f;

    //listening
    maxiBiquad bpf1;
    maxiBiquad bpf2;
    maxiBiquad bpf3;
    maxiBiquad bpf4;

    maxiEnvelopeFollowerF bpfEnv1;
    maxiEnvelopeFollowerF bpfEnv2;
    maxiEnvelopeFollowerF bpfEnv3;
    maxiEnvelopeFollowerF bpfEnv4;

    maxiDCBlocker dcb;

    OnePoleSmoother<kN_Params> smoother;

};




// Global objects
std::shared_ptr<IMLInterface> APP_SRAM interfaceIML;
std::shared_ptr<interfaceRL> APP_SRAM RLInterface;

std::shared_ptr<MIDIInOut> midi_interf;


std::shared_ptr<PAFSynthApp> __scratch_y("audio") audio_app;

// Inter-core communication
volatile bool APP_SRAM core_0_ready = false;
volatile bool APP_SRAM core_1_ready = false;
volatile bool APP_SRAM serial_ready = false;
volatile bool APP_SRAM interface_ready = false;


// We're only bound to the joystick inputs (x, y, rotate)
constexpr size_t kN_InputParams = 0;

void like(std::shared_ptr<interfaceRL> interface) {
        static APP_SRAM std::vector<String> likemsgs = {"Wow, incredible", "Awesome", "That's amazing", "Unbelievable+","I love it!!","More of this","Yes!!!!","A-M-A-Z-I-N-G"};
        String msg = likemsgs[rand() % likemsgs.size()];
        interface->storeExperience(1.f);
        Serial.println(msg);
        scr.post(msg);
}

void dislike(std::shared_ptr<interfaceRL> interface) {
        static APP_SRAM std::vector<String> dislikemsgs = {"Awful!","wtf? that sucks","Get rid of this sound","Totally shite","I hate this","Why even bother?","New sound please!","No, please no!!!","Thumbs down"};
        String msg = dislikemsgs[rand() % dislikemsgs.size()];
        interface->storeExperience(-1.f);
        Serial.println(msg);
        scr.post(msg);
}
void randomiseRL(std::shared_ptr<interfaceRL> interface) {
    interface->randomiseTheActor();
    interface->generateAction(true);
    Serial.println("The Actor is confused");
    scr.post("Actor: i'm confused");
}

void bind_RL_interface(std::shared_ptr<interfaceRL> interface)
{
    // Set up momentary switch callbacks
    MEMLNaut::Instance()->setMomA1Callback([interface] () {
        // static APP_SRAM std::vector<String> msgs = {"Wow, incredible", "Awesome", "That's amazing", "Unbelievable+","I love it!!","More of this","Yes!!!!","A-M-A-Z-I-N-G"};
        // String msg = msgs[rand() % msgs.size()];
        // interface->storeExperience(1.f);
        // Serial.println(msg);
        
        // scr.post(msg);
        like(interface);
    });
    MEMLNaut::Instance()->setMomA2Callback([interface] () {
        // static APP_SRAM std::vector<String> msgs = {"Awful!","wtf? that sucks","Get rid of this sound","Totally shite","I hate this","Why even bother?","New sound please!","No, please no!!!","Thumbs down"};
        // String msg = msgs[rand() % msgs.size()];
        // interface->storeExperience(-1.f);
        // Serial.println(msg);
        // scr.post(msg);
        dislike(interface);
    });
    MEMLNaut::Instance()->setMomB1Callback([interface] () {
        randomiseRL(interface);
        // interface->randomiseTheActor();
        // interface->generateAction(true);
        // Serial.println("The Actor is confused");
        // scr.post("Actor: i'm confused");
    });
    MEMLNaut::Instance()->setMomB2Callback([interface] () {
        interface->randomiseTheCritic();
        interface->generateAction(true);
        Serial.println("The Critic is confounded");
        scr.post("Critic: totally confounded");
    });
    // Set up ADC callbacks
    MEMLNaut::Instance()->setJoyXCallback([interface] (float value) {
        // interface->setState(0, value);
    });
    MEMLNaut::Instance()->setJoyYCallback([interface] (float value) {
        // interface->setState(1, value);
    });
    MEMLNaut::Instance()->setJoyZCallback([interface] (float value) {
        // interface->setState(2, value);
    });

    MEMLNaut::Instance()->setRVGain1Callback([interface] (float value) {
        AudioDriver::setDACVolume(value);
    });

    MEMLNaut::Instance()->setRVX1Callback([interface] (float value) {
        size_t divisor = 1 + (value * 100);
        String msg = "Optimise every " + String(divisor);
        scr.post(msg);
        interface->setOptimiseDivisor(divisor);
        Serial.println(msg);
    });
    

    // Set up loop callback
    MEMLNaut::Instance()->setLoopCallback([interface] () {
        interface->optimiseSometimes();
        interface->generateAction();
    });

  
}

void bind_IML_interface(std::shared_ptr<IMLInterface> interface)
{
    // Set up momentary switch callbacks
    MEMLNaut::Instance()->setMomA1Callback([interface] () {
        interface->Randomise();
    });
    MEMLNaut::Instance()->setMomA2Callback([interface] () {
        interface->ClearData();
    });

    // Set up toggle switch callbacks
    MEMLNaut::Instance()->setTogA1Callback([interface] (bool state) {
        interface->SetTrainingMode(state ? IMLInterface::TRAINING_MODE : IMLInterface::INFERENCE_MODE);
    });
    MEMLNaut::Instance()->setJoySWCallback([interface] (bool state) {
        interface->SaveInput(state ? IMLInterface::STORE_VALUE_MODE : IMLInterface::STORE_POSITION_MODE);
    });

    // Set up ADC callbacks
    MEMLNaut::Instance()->setJoyXCallback([interface] (float value) {
        interface->SetInput(0, value);
    });
    MEMLNaut::Instance()->setJoyYCallback([interface] (float value) {
        interface->SetInput(1, value);
    });
    MEMLNaut::Instance()->setJoyZCallback([interface] (float value) {
        interface->SetInput(2, value);
    });
    MEMLNaut::Instance()->setRVZ1Callback([interface] (float value) {
        // Scale value from 0-1 range to 1-3000
        value = 1.0f + (value * 2999.0f);
        interface->SetIterations(static_cast<size_t>(value));
    });

    // Set up loop callback
    MEMLNaut::Instance()->setLoopCallback([interface] () {
        interface->ProcessInput();
    });

    MEMLNaut::Instance()->setRVGain1Callback([interface] (float value) {
        AudioDriver::setDACVolume(value);
    });
}

enum MLMODES {IML, RL};  
MLMODES APP_SRAM mlMode = RL;


struct repeating_timer APP_SRAM timerDisplay;
inline bool __not_in_flash_func(displayUpdate)(__unused struct repeating_timer *t) {
    scr.update();
    return true;
}
  
void setup()
{

  LittleFS.begin();
  VFS.root(LittleFS);



    // FILE *fp = fopen("/thisfilelivesonflash.txt", "w");
    // fprintf(fp, "Hello!\n");
    // fclose(fp);

    scr.setup();
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    uint32_t seed = get_rosc_entropy_seed(32);
    srand(seed);

    Serial.begin(115200);
    // while (!Serial) {}
    Serial.println("Serial initialised.");
    WRITE_VOLATILE(serial_ready, true);
    // FILE *fp = fopen("/thisfilelivesonflash.txt", "r");
    // char line[128];
    // while (fgets(line, sizeof(line), fp)) {
    //     Serial.printf("%s", line);
    // }

    // fclose(fp);

    // Setup board
    MEMLNaut::Initialize();
    pinMode(33, OUTPUT);

    switch(mlMode) {
        case IML: {
            {
                auto temp_interface = std::make_shared<IMLInterface>();
                temp_interface->setup(kN_InputParams, PAFSynthApp::kN_Params);
                MEMORY_BARRIER();
                interfaceIML = temp_interface;
                MEMORY_BARRIER();
            }
            // Setup interface with memory barrier protection
            WRITE_VOLATILE(interface_ready, true);
            // Bind interface after ensuring it's fully initialized
            bind_IML_interface(interfaceIML);
            Serial.println("Bound IML interface to MEMLNaut.");
        }
        break;
        case RL: {
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
            bind_RL_interface(RLInterface);
            Serial.println("Bound RL interface to MEMLNaut.");
        }
        break;
    }

    midi_interf = std::make_shared<MIDIInOut>();
    midi_interf->Setup(4);
    midi_interf->SetMIDISendChannel(1);
    Serial.println("MIDI setup complete.");
    if (midi_interf) {
        midi_interf->SetCCCallback([RLInterface] (uint8_t cc_number, uint8_t cc_value) {
            Serial.printf("MIDI CC %d: %d\n", cc_number, cc_value);
            switch(cc_number) {
                case 1:
                {
                    like(RLInterface);
                    break;
                }
                case 2:
                {
                    dislike(RLInterface);
                    break;
                }
                case 3:
                {
                    randomiseRL(RLInterface);
                    break;
                }
                case 4:
                {
                    break;
                }
            };
        });
    }


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
