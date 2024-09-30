/// @file
///	@copyright	Copyright 2024 VERTIGO. All rights reserved.
///	@license	Use of this source code is governed by the MIT License found in the License.md file.

#include <iostream>
#include <thread>
#include <vector>
#include <cmath>
#include <cassert>

#include "c74_min.h"
#include "../shared/signal_routing_objects.h"
#include "c74_min_api.h"
#include "netherdream.h"

//struct EAD_Pnt_s dot[DOT_POINTS];


#ifndef M_PI // M_PI not defined on Windows
    #define M_PI 3.14159265358979323846
#endif

#define CIRCLE_POINTS						960
struct etherdream_point circleA[CIRCLE_POINTS];


class etherdream : public signal_routing_base<etherdream>, public sample_operator<5, 1> {

public:
	MIN_DESCRIPTION {"ETHERDREAM"};
	MIN_TAGS {"audio, routing"};
	MIN_AUTHOR {"Cycling '74"};
	MIN_RELATED {"panner~, matrix~"};

	inlet<>  X1{this, "(signal) X1"};
	inlet<>  Y1{this, "(signal) Y1"};
	inlet<>  R1{ this, "(signal) R1"};
	inlet<>  G1{ this, "(signal) G1" };
	inlet<>  B1{ this, "(signal) B1" };
	outlet<> out1{this, "(anything) dac names"};
    outlet<> out2{ this, "(anything) failures" };
    attribute<int> ndacA{ this, "devicenrA", -1};
    //attribute<int> dSample{ this, "downsample", 1 };

	void addA(sample x, sample y, sample r, sample g, sample b);
	void sendFrame();
        
    message<> setup{ this, "setup",
        MIN_FUNCTION {
            etherdream_lib_start();
            return {};
        }
    };

    message<> getdevices{ this, "getdevices", "get etherdreams",
    MIN_FUNCTION {
        //if (netherdream::Close());
        int cc = etherdream_dac_count();
        for (uint8_t i = 0; i < cc; i++) {
            out1.send(etherdream_get_id(etherdream_get(i)));
        }
        return {};
        }
    };

    message<> start{ this, "start", "starts etherdream",
        MIN_FUNCTION {
            int cc = etherdream_dac_count();
            deviceOpenA = false;
            if (cc > 0){
                netherdream* a = etherdream_get(ndacA);

                if (etherdream_connect(a) == -1) {
                  out2.send("Etherdream A cant open");
                }else{
                    deviceOpenA = true;
                    out2.send("Etherdream A started");
                }
            }
            return {};
        }
    };

    message<> bang{ this, "bang", "Post the greeting.",
        MIN_FUNCTION {
            out2.send(numberPoints);
        return {};
        }
    };

	samples<1> operator()(sample in1, sample in2, sample in3, sample in4, sample in5) {
            addA(in1, in2, in3, in4, in5);
            if (pointA == 960) {
                sendFrame();
            }
	    return {};
	}
    private:

        uint16_t pointA = 0;
        uint8_t downSample = 0;
        uint8_t dSample = 1;
        bool rising = true;
        uint16_t numberPoints = 0;
        bool deviceOpenA = false;
        struct netherdream* a;

};
MIN_EXTERNAL(etherdream);

void etherdream::addA(sample x, sample y, sample r, sample g, sample b) {
    if (pointA < 960) {
        struct etherdream_point* ptA = &circleA[pointA];
        ptA->x = x * std::numeric_limits<int16_t>::max();
        ptA->y = y * std::numeric_limits<int16_t>::max();
        ptA->r = r * std::numeric_limits<int16_t>::max();
        ptA->g = g * std::numeric_limits<int16_t>::max();
        ptA->b = b * std::numeric_limits<int16_t>::max();
        ptA->i = std::numeric_limits<int16_t>::max();
        pointA += 1;
    }
}

void etherdream::sendFrame() {
    if (deviceOpenA) {
        if (pointA > 0) {
            if (etherdream_is_ready(a) == 1) {
                etherdream_write(a, circleA, pointA, 48000, 1);
            }
            numberPoints = pointA;
        }
        else {
            numberPoints = 0;
        }
    }
    pointA = 0;
}
