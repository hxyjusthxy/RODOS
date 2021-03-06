#include "rodos.h"

static Application module01("Priority");

class HighPriorityThread: public Thread{
public:
  HighPriorityThread() : Thread("HiPriority", 25) { }

  void init() { 
    PRINTF(" hipri = '*'"); 
  }

  void run() {
    while(1) {
      PRINTF("*");
      AT(NOW() + 1*SECONDS);
    }
  }
};


class LowPriorityThread: public Thread {
public:
  LowPriorityThread() : Thread("LowPriority", 10) { }

  void init() { 
    PRINTF(" lopri = '.'"); 
  }

  void run() {
    long long cnt = 0;

    while(1) {
      cnt++;
      if (cnt % 10000000 == 0) {
        PRINTF(".");
      }
      if (cnt % 500000000 == 0){ break; } //leave first loop
    }

    PRIORITY_CEILING { //no more interruptions
      while(1) {
        cnt++;
        if (cnt % 10000000 == 0) {
          PRINTF(".");
        }
      }
    }
  }
};

HighPriorityThread highPriorityThread;
LowPriorityThread  lowPriorityThread;


