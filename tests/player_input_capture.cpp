#include "player_input_capture.h"
#include "player_input_keyboard_pad_gate.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void check(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
static bool equal(double a,double b) {return std::abs(a-b)<1e-10;}
static void axes(void* controller,int32_t x) {
    for(unsigned axis=0;axis<6;++axis)mag_input_capture_controller_axis(controller,axis,axis==MAG_INPUT_LOOK_X?x:0);
}
int main() {try {
    int first=0,other=0;void* c=&first;void* c1=&other;
    mag_input_capture_controller_open(c,0);mag_input_capture_controller_open(c1,1);
    check(mag_input_capture_poll_begin(1),"actual poll begins once");
    mag_input_capture_backend_begin();mag_input_capture_controller_attached(c,1);axes(c,16384);
    mag_input_capture_controller_axis(c,MAG_INPUT_MOVE_Y,-32768);
    mag_input_capture_controller_axis(c,MAG_INPUT_RIGHT_TRIGGER,32767);
    mag_input_capture_controller_button(c,3,1);axes(c1,-32768);mag_input_capture_controller_button(c1,4,1);
    MagInputControllerSample sample{};
    check(mag_input_capture_poll_complete(1,&sample),"whole current slot-zero raw sample completes");
    check(sample.connected && sample.input_sample_sequence==1 && sample.backend_sequence==1,"sample is associated with this native poll and backend refresh");
    check(equal(sample.look_x,16384./32767) && sample.move_y==-1 && sample.right_trigger==1 && sample.buttons==8,"pre-quantized axes/buttons preserved and other port ignored");
    sample.input_sample_sequence=99;
    check(!mag_input_capture_poll_complete(1,&sample) && sample.input_sample_sequence==99,"duplicate completion cannot deliver old data again");
    check(!mag_input_capture_poll_begin(1),"retired poll key cannot reacquire newer backend data");
    check(mag_input_capture_poll_begin(2),"next native poll begins");
    check(!mag_input_capture_poll_begin(3),"nested poll cannot replace active ownership");
    check(!mag_input_capture_poll_complete(2,&sample),"missing backend update cannot masquerade as a fresh sample");
    check(mag_input_capture_poll_begin(3),"fresh key after failed update is accepted");
    mag_input_capture_backend_begin();
    check(mag_input_capture_poll_complete(3,&sample) && !sample.connected && sample.look_x==0,"a real backend poll with no port-zero device reports disconnection");
    check(mag_input_capture_poll_begin(4),"new poll may contain multiple backend refreshes");
    mag_input_capture_backend_begin();mag_input_capture_controller_attached(c,1);axes(c,16384);
    mag_input_capture_backend_begin();mag_input_capture_controller_attached(c,1);axes(c,-16384);
    check(mag_input_capture_poll_complete(4,&sample) && sample.backend_sequence==4 && sample.look_x==-.5,"latest whole backend refresh wins without combining poll generations");
    check(mag_input_capture_poll_begin(5),"partial poll begins");
    mag_input_capture_backend_begin();mag_input_capture_controller_attached(c,1);mag_input_capture_controller_axis(c,MAG_INPUT_LOOK_X,32767);
    check(!mag_input_capture_poll_complete(5,&sample),"partial axis set cannot reuse earlier values");
    check(mag_input_capture_poll_begin(6),"disconnect poll begins");
    mag_input_capture_backend_begin();mag_input_capture_controller_attached(c,1);axes(c,32767);mag_input_capture_controller_close(c);
    check(mag_input_capture_poll_complete(6,&sample) && !sample.connected && !sample.buttons && sample.look_x==0,"mid-poll disconnect revokes old raw contribution");
    check(mag_input_capture_poll_begin(7),"abortable poll begins");mag_input_capture_poll_abort(7);
    check(!mag_input_capture_poll_complete(7,&sample),"aborted poll is unavailable");

    // The legacy keyboard pad is muted only while owned and not live; live alone mutes nothing.
    check(!mag_input_capture_keyboard_owned() && !mag_input_capture_keyboard_pad_muted(),"nothing owned at start");
    mag_input_capture_set_keyboard_pad_live(1);
    check(!mag_input_capture_keyboard_pad_muted(),"live without ownership mutes nothing");
    mag_input_capture_set_keyboard_owner(1);
    check(mag_input_capture_keyboard_owned() && !mag_input_capture_keyboard_pad_muted(),"an owned keyboard with a live pad (in-match menu)");
    mag_input_capture_set_keyboard_pad_live(0);
    check(mag_input_capture_keyboard_pad_muted(),"an owned keyboard without a live pad mutes it (on foot)");
    mag_input_capture_set_keyboard_owner(0);
    check(!mag_input_capture_keyboard_pad_muted(),"releasing ownership unmutes the pad");

    // The gate: every input counts before the pad was ever muted; after a mute, an input counts
    // only once seen released.
    static MagKeyboardPadGate gate;
    mag_keyboard_pad_gate_begin(&gate,0);
    check(mag_keyboard_pad_gate_input(&gate,7,1),"a held key counts before any mute (no adapter: unchanged)");
    mag_keyboard_pad_gate_begin(&gate,1);
    check(!mag_keyboard_pad_gate_input(&gate,7,1) && !mag_keyboard_pad_gate_input(&gate,8,0),"nothing counts while muted");
    mag_keyboard_pad_gate_begin(&gate,0);
    check(!mag_keyboard_pad_gate_input(&gate,7,1),"a key held across the unmute does not count");
    check(!mag_keyboard_pad_gate_input(&gate,7,1),"nor while it stays held");
    check(!mag_keyboard_pad_gate_input(&gate,8,0) && mag_keyboard_pad_gate_input(&gate,8,1),"a key seen released counts on its next press");
    check(!mag_keyboard_pad_gate_input(&gate,7,0) && mag_keyboard_pad_gate_input(&gate,7,1),"the held key counts after its release");
    check(!mag_keyboard_pad_gate_input(&gate,300,1),"out-of-range inputs never count");
    mag_keyboard_pad_gate_begin(&gate,1);mag_keyboard_pad_gate_begin(&gate,0);
    check(!mag_keyboard_pad_gate_input(&gate,8,1),"each new mute requires a fresh release");
    // A snapshot: held buttons and off-centre sticks read released and centred until let go.
    uint16_t buttons=0x0005;uint8_t lx=0,ly=128,rx=200,ry=128;
    mag_keyboard_pad_gate_snapshot(&gate,1,&buttons,&lx,&ly,&rx,&ry);
    check(buttons==0 && lx==128 && rx==128,"a muted snapshot is neutral");
    buttons=0x0005;lx=0;rx=200;
    mag_keyboard_pad_gate_snapshot(&gate,0,&buttons,&lx,&ly,&rx,&ry);
    check(buttons==0 && lx==128 && rx==128,"held buttons and sticks wait for a release after the unmute");
    buttons=0x0001;lx=128;rx=200;
    mag_keyboard_pad_gate_snapshot(&gate,0,&buttons,&lx,&ly,&rx,&ry);
    check(buttons==0 && lx==128 && rx==128,"still held");
    buttons=0x0003;lx=0;rx=128;
    mag_keyboard_pad_gate_snapshot(&gate,0,&buttons,&lx,&ly,&rx,&ry);
    check(buttons==0x0002 && lx==0 && rx==128,"released inputs count again, still-held ones wait");
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
