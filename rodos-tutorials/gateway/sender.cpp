#include "rodos.h"
#include "demo_topics.h"

/******************************/

class MyPublisher : public Thread {
public:
    MyPublisher() : Thread("sender") { }
    void run () {
        int32_t cnt1 = 8000;
        int32_t cnt2 = 9000;
        TIME_LOOP(0, 1000*MILLISECONDS) {
            PRINTF("at %9.3f sending %d %d\n",  SECONDS_NOW(), cnt1, cnt2);
            counter1.publish(cnt1);
            counter2.publish(cnt2);
            cnt1++;
            cnt2++;
        }
    }
} myPublisher;


