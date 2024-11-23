// Copyright (c) 2022 Robin Davies
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "pch.h"
#include "catch.hpp"
#include <sstream>
#include <cstdint>
#include <string>
#include <iostream>

#include "PiPedalAlsa.hpp"

using namespace pipedal;
using namespace std;

static void DiscoveryTest()
{
    cout << "--- Discovery" << endl;
    PiPedalAlsaDevices devices;
    auto result = devices.GetAlsaDevices();
    std::cout << result.size() << " ALSA devices found." << std::endl;

    auto midiInputDevices = GetAlsaMidiInputDevices();
    std::cout << midiInputDevices.size() << " ALSA MIDI input devices found." << std::endl;
    auto midiOutputDevices = GetAlsaMidiOutputDevices();
    std::cout << midiOutputDevices.size() << " ALSA MIDI output devices found." << std::endl;


}


TEST_CASE( "ALSA Test", "[pipedal_alsa_test][Build][Dev]" ) {
    DiscoveryTest();


}



