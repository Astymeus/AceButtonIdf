#include <esp_log.h>
#include "include/EventTracker.h"

namespace ace_button {
namespace testing {

const char* TAG = "AWW Event";

void EventRecord::printTo() const {
  ESP_LOGI.print(TAG, "pin: %d; eventType: %d; buttonState: %d", mPin, mEventType, mButtonState);
}

void EventTracker::printTo() const {
  for (int i = 0; i < mNumEvents; i++) {
    ESP_LOGI.print(TAG, "%d:", i);
    mRecords[i].printTo();
  }
}

}
}
