#include "player_desktop_capture.h"
#include <stdexcept>
#include <thread>
using namespace mag;
static void check(bool b) {if(!b)throw std::runtime_error("desktop capture contract failed");}
int main() {
    DesktopCaptureGate gate;check(!gate.accepts(gate.stamp()));gate.publish(true,true);
    DesktopCaptureGate::Stamp queued;std::thread device([&]{queued=gate.stamp();});device.join();
    check(gate.accepts(queued,true));gate.publish(false,false);gate.publish(true,true);
    check(!gate.accepts(queued) && !gate.accepts(queued,true));
    auto current=gate.stamp();gate.publish(true,true);check(gate.accepts(current,true));
    gate.publish(true,false);check(!gate.accepts(current));current=gate.stamp();
    check(gate.accepts(current) && !gate.accepts(current,true));gate.invalidate();check(!gate.accepts(current));
    DesktopButtonAggregation buttons;
    check(buttons.update(0,11,true)==true);check(!buttons.update(0,12,true));
    check(!buttons.update(0,11,false));check(buttons.update(0,12,false)==false);
    check(!buttons.update(0,11,false));check(buttons.update(1,11,true)==true);
    check(buttons.update(0,11,true)==true);buttons.clear();check(!buttons.update(0,11,false));
}
