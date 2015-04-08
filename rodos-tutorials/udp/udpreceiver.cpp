
#include "rodos.h"

static Application  receiverName("UDPReceiver");

static UDPIn in(5001);

class Receiver : public Thread {

	char userData[2000];

	void run () {
		TIME_LOOP(1*SECONDS, 20*MILLISECONDS) {
			while(int len = in.get(userData, 200)) {
				PRINTF("UDP Receiver: %d, %s\n", len, userData);
			}
		}
	}
};

/******************************/

static Receiver receiver;

