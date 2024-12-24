/*
MIT License

Copyright (c) 2018 Brian T. Park

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "include/AceButton.h"

namespace ace_button {

//-----------------------------------------------------------------------------

// Macros to perform compile-time assertions. See
// https://www.embedded.com/electronics-blogs/programming-pointers/4025549/Catching-errors-early-with-compile-time-assertions
// and https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html
#define CONCAT_(x, y) x##y
#define CONCAT(x,y) CONCAT_(x,y)
#define COMPILE_TIME_ASSERT(cond, msg) \
    extern char CONCAT(compile_time_assert, __LINE__)[(cond) ? 1 : -1];

// Check that the Arduino constants HIGH and LOW are defined to be 1 and 0,
// respectively. Otherwise, this library won't work.
COMPILE_TIME_ASSERT(HIGH == 1, "HIGH must be 1")
COMPILE_TIME_ASSERT(LOW == 0, "LOW must be 0")

// On boards using the new PinStatus API, check that kButtonStateUnknown is
// different from all other PinStatus enums.
#if ARDUINO_API_VERSION >= 10000
  COMPILE_TIME_ASSERT(\
    AceButton::kButtonStateUnknown != LOW \
    && AceButton::kButtonStateUnknown != HIGH \
    && AceButton::kButtonStateUnknown != CHANGE \
    && AceButton::kButtonStateUnknown != FALLING \
    && AceButton::kButtonStateUnknown != RISING, \
    "kButtonStateUnknown conflicts with PinStatus enum")
#endif

//-----------------------------------------------------------------------------

static const char sEventPressed[] = "Pressed";
static const char sEventReleased[] = "Released";
static const char sEventClicked[] = "Clicked";
static const char sEventDoubleClicked[] = "DoubleClicked";
static const char sEventLongPressed[] = "LongPressed";
static const char sEventRepeatPressed[] = "RepeatPressed";
static const char sEventLongReleased[] = "LongReleased";
static const char sEventHeartBeat[] = "HeartBeat";
static const char sEventUnknown[] = "(unknown)";

static const char* const sEventNames[] = {
  sEventPressed,
  sEventReleased,
  sEventClicked,
  sEventDoubleClicked,
  sEventLongPressed,
  sEventRepeatPressed,
  sEventLongReleased,
  sEventHeartBeat,
};

const char* AceButton::eventName(uint8_t event) {
  // const char* name = (event >= sizeof(sEventNames) / sizeof(const char*))
  //     ? sEventUnknown
  //     : (const char*) ((void*)__LPM_word((uint16_t)(&sEventNames + event)));
  return sEventUnknown;
}

//-----------------------------------------------------------------------------

void AceButton::init(uint8_t pin, uint8_t defaultReleasedState, uint8_t id) {
  mPin = pin;
  mId = id;
  mFlags = 0;
  mLastButtonState = kButtonStateUnknown;
  setDefaultReleasedState(defaultReleasedState);
}

void AceButton::init(ButtonConfig* buttonConfig, uint8_t pin,
    uint8_t defaultReleasedState, uint8_t id) {
  mButtonConfig = buttonConfig;
  init(pin, defaultReleasedState, id);
}

void AceButton::setDefaultReleasedState(uint8_t state) {
  if (state == HIGH) {
    mFlags |= kFlagDefaultReleasedState;
  } else {
    mFlags &= ~kFlagDefaultReleasedState;
  }
}

uint8_t AceButton::getDefaultReleasedState() const {
  return (mFlags & kFlagDefaultReleasedState) ? HIGH : LOW;
}

// NOTE: It would be interesting to rewrite the check() method using a Finite
// State Machine.
void AceButton::check() {
  int buttonState = mButtonConfig->readButton(mPin);
  checkState(buttonState);
}

void AceButton::checkState(int buttonState) {
  // Retrieve the current time just once and use that in the various checkXxx()
  // functions below. This provides some robustness of the various timing
  // algorithms even if one of the event handlers takes more time than the
  // threshold time limits such as 'debounceDelay' or longPressDelay'.
  int64_t now = mButtonConfig->getClock();

  // Send heart beat if enabled and needed. Purposely placed outside of the
  // checkDebounced() guard so that it can fire regardless of the state of the
  // debouncing logic.
  checkHeartBeat(now);

  // Debounce the button, and send any events detected.
  if (checkDebounced(now, buttonState)) {
    // check if the button was initialized (i.e. UNKNOWN state)
    if (checkInitialized(buttonState)) {
      checkEvent(now, buttonState);
    }
  }
}

void AceButton::checkEvent(int64_t now, int buttonState) {
  // We need to remove orphaned clicks even if just Click is enabled. It is not
  // sufficient to do this for just DoubleClick. That's because it's possible
  // for a Clicked event to be generated, then 65.536 seconds later, the
  // ButtonConfig could be changed to enable DoubleClick. (Such real-time change
  // of ButtonConfig is not recommended, but is sometimes convenient.) If the
  // orphaned click is not cleared, then the next Click would be errorneously
  // considered to be a DoubleClick. Therefore, we must clear the orphaned click
  // even if just the Clicked event is enabled.
  //
  // We also need to check of any postponed clicks that got generated when
  // kFeatureSuppressClickBeforeDoubleClick was enabled.
  if (mButtonConfig->isFeature(ButtonConfig::kFeatureClick) ||
      mButtonConfig->isFeature(ButtonConfig::kFeatureDoubleClick)) {
    checkPostponedClick(now);
    checkOrphanedClick(now);
  }

  if (mButtonConfig->isFeature(ButtonConfig::kFeatureLongPress)) {
    checkLongPress(now, buttonState);
  }
  if (mButtonConfig->isFeature(ButtonConfig::kFeatureRepeatPress)) {
    checkRepeatPress(now, buttonState);
  }
  if (buttonState != getLastButtonState()) {
    checkChanged(now, buttonState);
  }
}

bool AceButton::checkDebounced(int64_t now, int buttonState) {
  if (isFlag(kFlagDebouncing)) {

    // NOTE: This is a bit tricky. The elapsedTime will be valid even if the
    // uint16_t representation of 'now' rolls over so that (now <
    // mLastDebounceTime). This is true as long as the 'unsigned long'
    // representation of 'now' is < (65536 + mLastDebounceTime). We need to cast
    // this expression into an uint16_t before doing the '>=' comparison below
    // for compatability with processors whose sizeof(int) == 4 instead of 2.
    // For those processors, the expression (now - mLastDebounceTime >=
    // getDebounceDelay()) won't work because the terms in the expression get
    // promoted to an (int).
    int64_t elapsedTime = now - mLastDebounceTime;

    bool isDebouncingTimeOver =
        (elapsedTime >= mButtonConfig->getDebounceDelay());

    if (isDebouncingTimeOver) {
      clearFlag(kFlagDebouncing);
      return true;
    } else {
      return false;
    }
  } else {
    // Currently not in debouncing phase. Check for a button state change. This
    // will also detect a transition from kButtonStateUnknown to HIGH or LOW.
    if (buttonState == getLastButtonState()) {
      // no change, return immediately
      return true;
    }

    // button has changed so, enter debouncing phase
    setFlag(kFlagDebouncing);
    mLastDebounceTime = now;
    return false;
  }
}

bool AceButton::checkInitialized(uint16_t buttonState) {
  if (mLastButtonState != kButtonStateUnknown) {
    return true;
  }

  // If transitioning from the initial "unknown" button state, just set the last
  // valid button state, but don't fire off the event handler. This handles the
  // case where a momentary switch is pressed down, then the board is rebooted.
  // When the board comes up, it should not fire off the event handler. This
  // also handles the case of a 2-position switch set to the "pressed"
  // position, and the board is rebooted.
  mLastButtonState = buttonState;
  return false;
}

void AceButton::checkLongPress(int64_t now, int buttonState) {
  if (buttonState == getDefaultReleasedState()) {
    return;
  }

  if (isFlag(kFlagPressed) && !isFlag(kFlagLongPressed)) {
    int64_t elapsedTime = now - mLastPressTime;
    if (elapsedTime >= mButtonConfig->getLongPressDelay()) {
      setFlag(kFlagLongPressed);
      handleEvent(kEventLongPressed);
    }
  }
}

void AceButton::checkRepeatPress(int64_t now, int buttonState) {
  if (buttonState == getDefaultReleasedState()) {
    return;
  }

  if (isFlag(kFlagPressed)) {
    if (isFlag(kFlagRepeatPressed)) {
      int64_t elapsedTime = now - mLastRepeatPressTime;
      if (elapsedTime >= mButtonConfig->getRepeatPressInterval()) {
        handleEvent(kEventRepeatPressed);
        mLastRepeatPressTime = now;
      }
    } else {
      int64_t elapsedTime = now - mLastPressTime;
      if (elapsedTime >= mButtonConfig->getRepeatPressDelay()) {
        setFlag(kFlagRepeatPressed);
        // Trigger the RepeatPressed immedidately, instead of waiting until the
        // first getRepeatPressInterval() has passed.
        handleEvent(kEventRepeatPressed);
        mLastRepeatPressTime = now;
      }
    }
  }
}

void AceButton::checkChanged(int64_t now, int buttonState) {
  mLastButtonState = buttonState;
  checkPressed(now, buttonState);
  checkReleased(now, buttonState);
}

void AceButton::checkPressed(int64_t now, int buttonState) {
  if (buttonState == getDefaultReleasedState()) {
    return;
  }

  // button was pressed
  mLastPressTime = now;
  setFlag(kFlagPressed);
  handleEvent(kEventPressed);
}

void AceButton::checkReleased(int64_t now, int buttonState) {
  if (buttonState != getDefaultReleasedState()) {
    return;
  }

  // Check for click (before sending off the Released event).
  // Make sure that we don't clearPressed() before calling this.
  if (mButtonConfig->isFeature(ButtonConfig::kFeatureClick)
      || mButtonConfig->isFeature(ButtonConfig::kFeatureDoubleClick)) {
    checkClicked(now);
  }

  // Save whether this was generated from a long press.
  bool wasLongPressed = isFlag(kFlagLongPressed);

  // Check if Released events are suppressed.
  bool suppress =
      ((isFlag(kFlagLongPressed) &&
          mButtonConfig->
              isFeature(ButtonConfig::kFeatureSuppressAfterLongPress)) ||
      (isFlag(kFlagRepeatPressed) &&
          mButtonConfig->
              isFeature(ButtonConfig::kFeatureSuppressAfterRepeatPress)) ||
      (isFlag(kFlagClicked) &&
          mButtonConfig->isFeature(ButtonConfig::kFeatureSuppressAfterClick)) ||
      (isFlag(kFlagDoubleClicked) &&
          mButtonConfig->
              isFeature(ButtonConfig::kFeatureSuppressAfterDoubleClick)));

  // Button was released, so clear current flags. Note that the compiler will
  // optimize the following 4 statements to be equivalent to this single one:
  //    mFlags &= ~kFlagPressed & ~kFlagDoubleClicked & ~kFlagLongPressed
  //        & ~kFlagRepeatPressed;
  clearFlag(kFlagPressed);
  clearFlag(kFlagDoubleClicked);
  clearFlag(kFlagLongPressed);
  clearFlag(kFlagRepeatPressed);

  // Fire off a Released event, unless suppressed. Replace Released with
  // LongReleased if this was a LongPressed.
  if (suppress) {
    if (wasLongPressed) {
      handleEvent(kEventLongReleased);
    }
  } else {
    handleEvent(kEventReleased);
  }
}

void AceButton::checkClicked(int64_t now) {
  int64_t elapsedTime = now - mLastPressTime;
  printf("checkClicked => mLastPressTime: %lld\n", mLastPressTime);
  printf("checkClicked => elapsedTime: %lld\n", elapsedTime);
  if (elapsedTime >= mButtonConfig->getClickDelay()) {
    clearFlag(kFlagClicked);
    return;
  }

  // check for double click
  if (mButtonConfig->isFeature(ButtonConfig::kFeatureDoubleClick)) {
    checkDoubleClicked(now);
  }

  // Suppress a second click (both buttonState change and event message) if
  // double-click detected, which has the side-effect of preventing 3 clicks
  // from generating another double-click at the third click.
  if (isFlag(kFlagDoubleClicked)) {
    clearFlag(kFlagClicked);
    return;
  }

  // we got a single click
  mLastClickTime = now;
  setFlag(kFlagClicked);
  if (mButtonConfig->isFeature(
      ButtonConfig::kFeatureSuppressClickBeforeDoubleClick)) {
    setFlag(kFlagClickPostponed);
  } else {
    handleEvent(kEventClicked);
  }
}

void AceButton::checkDoubleClicked(int64_t now) {
  if (!isFlag(kFlagClicked)) {
    clearFlag(kFlagDoubleClicked);
    return;
  }

  int64_t elapsedTime = now - mLastClickTime;
  printf("checkDoubleClicked => elapsedTime: %lld\n", elapsedTime);
  if (elapsedTime >= mButtonConfig->getDoubleClickDelay()) {
    clearFlag(kFlagDoubleClicked);
    // There should be no postponed Click at this point because
    // checkPostponedClick() should have taken care of it.
    return;
  }

  // If there was a postponed click, suppress it because it could only have been
  // postponed if kFeatureSuppressClickBeforeDoubleClick was enabled. If we got
  // to this point, there was a DoubleClick, so we must suppress the first
  // Click as requested.
  if (isFlag(kFlagClickPostponed)) {
    clearFlag(kFlagClickPostponed);
  }
  setFlag(kFlagDoubleClicked);
  handleEvent(kEventDoubleClicked);
}

void AceButton::checkOrphanedClick(int64_t now) {
  // The amount of time which must pass before a click is determined to be
  // orphaned and reclaimed. If only DoubleClicked is supported, then I think
  // just getDoubleClickDelay() is correct. No other higher level event uses the
  // first Clicked event. If TripleClicked becomes supported, I think
  // orphanedClickDelay will be either (2 * getDoubleClickDelay()) or
  // (getDoubleClickDelay() + getTripleClickDelay()), depending on whether the
  // TripleClick has an independent delay time, or reuses the DoubleClick delay
  // time. But I'm not sure that I've thought through all the details.
  int64_t orphanedClickDelay = mButtonConfig->getDoubleClickDelay();

  int64_t elapsedTime = now - mLastClickTime;
  if (isFlag(kFlagClicked) && (elapsedTime >= orphanedClickDelay)) {
    clearFlag(kFlagClicked);
  }
}

void AceButton::checkPostponedClick(int64_t now) {
  int64_t postponedClickDelay = mButtonConfig->getDoubleClickDelay();
  int64_t elapsedTime = now - mLastClickTime;
  if (isFlag(kFlagClickPostponed) && elapsedTime >= postponedClickDelay) {
    handleEvent(kEventClicked);
    clearFlag(kFlagClickPostponed);
  }
}

void AceButton::checkHeartBeat(int64_t now) {
  if (! mButtonConfig->isFeature(ButtonConfig::kFeatureHeartBeat)) return;

  // On first call, set the last heart beat time.
  if (! isFlag(kFlagHeartRunning)) {
    setFlag(kFlagHeartRunning);
    mLastHeartBeatTime = now;
    return;
  }

  int64_t elapsedTime = now - mLastHeartBeatTime;
  if (elapsedTime >= mButtonConfig->getHeartBeatInterval()) {
    // This causes the kEventHeartBeat to be sent with the last validated button
    // state, not the current button state. I think that makes more sense, but
    // there might be situations where it doesn't.
    handleEvent(kEventHeartBeat);
    mLastHeartBeatTime = now;
  }
}

void AceButton::handleEvent(uint8_t eventType) {
  mButtonConfig->dispatchEvent(this, eventType, getLastButtonState());
}

}
