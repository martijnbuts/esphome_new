#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "esphome/components/usb_bluetooth/a2dp_logic.h"

namespace esphome::usb_bluetooth::testing {

struct StepCase {
  const char *name;
  A2dpState state;
  bool wanted;
  bool may_connect;
  uint32_t now;
  uint32_t last_try;
  uint32_t last_wanted;
  A2dpState next;
  A2dpAction action;
};

static const StepCase STEP_CASES[] = {
    {"no_speaker_or_searching", A2DP_IDLE, false, false, 500, 0, 0, A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE},
    {"first_connect", A2DP_IDLE, false, true, 500, 0, 0, A2DP_CONNECTING, A2dpAction::A2DP_ACTION_CONNECT},
    {"retry_waits", A2DP_IDLE, false, true, 9000, 1000, 0, A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE},
    {"retry_after_interval", A2DP_IDLE, false, true, 11000, 1000, 0, A2DP_CONNECTING, A2dpAction::A2DP_ACTION_CONNECT},
    {"retry_across_wrap", A2DP_IDLE, false, true, 5000, 0xFFFFF000u, 0, A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE},
    {"connecting_waits", A2DP_CONNECTING, false, true, 20000, 1000, 0, A2DP_CONNECTING, A2dpAction::A2DP_ACTION_NONE},
    {"connecting_gives_up", A2DP_CONNECTING, false, true, 32000, 1000, 0, A2DP_IDLE, A2dpAction::A2DP_ACTION_NONE},
    {"connected_idle", A2DP_CONNECTED, false, true, 1000, 0, 0, A2DP_CONNECTED, A2dpAction::A2DP_ACTION_NONE},
    {"connected_audio_checks", A2DP_CONNECTED, true, true, 1000, 0, 1000, A2DP_CONNECTED,
     A2dpAction::A2DP_ACTION_CHECK_READY},
    {"ready_starts", A2DP_READY, true, true, 1000, 0, 1000, A2DP_STARTING, A2dpAction::A2DP_ACTION_START},
    {"starting_waits", A2DP_STARTING, true, true, 1000, 0, 1000, A2DP_STARTING, A2dpAction::A2DP_ACTION_NONE},
    {"streaming_audio", A2DP_STREAMING, true, true, 9000, 0, 9000, A2DP_STREAMING, A2dpAction::A2DP_ACTION_NONE},
    {"streaming_short_gap", A2DP_STREAMING, false, true, 9000, 0, 7000, A2DP_STREAMING, A2dpAction::A2DP_ACTION_NONE},
    {"streaming_suspends", A2DP_STREAMING, false, true, 9000, 0, 5000, A2DP_SUSPENDING,
     A2dpAction::A2DP_ACTION_SUSPEND},
    {"suspending_waits", A2DP_SUSPENDING, true, true, 9000, 0, 9000, A2DP_SUSPENDING, A2dpAction::A2DP_ACTION_NONE},
    {"suspended_idle", A2DP_SUSPENDED, false, true, 9000, 0, 0, A2DP_SUSPENDED, A2dpAction::A2DP_ACTION_NONE},
    {"suspended_audio_resumes", A2DP_SUSPENDED, true, true, 9000, 0, 9000, A2DP_CONNECTED,
     A2dpAction::A2DP_ACTION_NONE},
};

class A2dpStepTest : public ::testing::TestWithParam<StepCase> {};

TEST_P(A2dpStepTest, NextStateAndAction) {
  const StepCase &c = GetParam();
  const A2dpStep step = a2dp_step(c.state, c.wanted, c.may_connect, c.now, c.last_try, c.last_wanted);
  EXPECT_EQ(step.state, c.next);
  EXPECT_EQ(step.action, c.action);
}

INSTANTIATE_TEST_SUITE_P(UsbBluetooth, A2dpStepTest, ::testing::ValuesIn(STEP_CASES),
                         [](const ::testing::TestParamInfo<StepCase> &info) { return std::string(info.param.name); });

// Audio that keeps coming from the first connection on streams without a gap.
TEST(A2dpStepSequence, ConnectThenStreamThenSuspend) {
  A2dpState state = A2DP_IDLE;
  A2dpStep step = a2dp_step(state, true, true, 1000, 0, 1000);
  EXPECT_EQ(step.action, A2dpAction::A2DP_ACTION_CONNECT);
  // The connection and media acknowledgements come from Bluedroid
  state = A2DP_CONNECTED;
  step = a2dp_step(state, true, true, 2000, 1000, 2000);
  EXPECT_EQ(step.action, A2dpAction::A2DP_ACTION_CHECK_READY);
  state = A2DP_READY;
  step = a2dp_step(state, true, true, 3000, 1000, 3000);
  EXPECT_EQ(step.state, A2DP_STARTING);
  state = A2DP_STREAMING;
  step = a2dp_step(state, false, true, 6000, 1000, 3000);
  EXPECT_EQ(step.state, A2DP_STREAMING);
  step = a2dp_step(state, false, true, 6001, 1000, 3000);
  EXPECT_EQ(step.action, A2dpAction::A2DP_ACTION_SUSPEND);
}

struct VolumeCase {
  float level;
  uint8_t volume;
};

class AvrcVolumeTest : public ::testing::TestWithParam<VolumeCase> {};

TEST_P(AvrcVolumeTest, Scale) { EXPECT_EQ(avrc_volume(GetParam().level), GetParam().volume); }

INSTANTIATE_TEST_SUITE_P(UsbBluetooth, AvrcVolumeTest,
                         ::testing::Values(VolumeCase{0.0f, 0}, VolumeCase{0.3f, 38}, VolumeCase{0.5f, 64},
                                           VolumeCase{1.0f, 127}, VolumeCase{-0.5f, 0}, VolumeCase{1.5f, 127}));

TEST(A2dpScale, HalfGain) {
  std::vector<int16_t> samples{1000, -1000, 32767, -32768, 0, 1};
  a2dp_scale(samples.data(), samples.size(), 0.5f);
  EXPECT_EQ(samples, (std::vector<int16_t>{500, -500, 16383, -16384, 0, 0}));
}

TEST(A2dpScale, ZeroGainSilences) {
  std::vector<int16_t> samples{1000, -1000, 32767, -32768};
  a2dp_scale(samples.data(), samples.size(), 0.0f);
  EXPECT_EQ(samples, (std::vector<int16_t>(4, 0)));
}

TEST(A2dpScale, OnlyCountSamples) {
  std::vector<int16_t> samples{1000, 1000, 1000};
  a2dp_scale(samples.data(), 2, 0.5f);
  EXPECT_EQ(samples, (std::vector<int16_t>{500, 500, 1000}));
}

static const uint8_t SPEAKER_ADDRESS[6] = {0x4C, 0x72, 0x74, 0x24, 0xC8, 0x05};

TEST(FormatSpeaker, NameAndAddress) {
  char text[96];
  format_speaker(text, sizeof(text), "SBL 4 A2", SPEAKER_ADDRESS);
  EXPECT_STREQ(text, "SBL 4 A2 (4C:72:74:24:C8:05)");
}

TEST(FormatSpeaker, AddressWhileNameUnknown) {
  char text[96];
  format_speaker(text, sizeof(text), "", SPEAKER_ADDRESS);
  EXPECT_STREQ(text, "4C:72:74:24:C8:05");
}

TEST(FormatSpeaker, LongNameIsCut) {
  char text[16];
  format_speaker(text, sizeof(text), "A very long speaker name", SPEAKER_ADDRESS);
  EXPECT_EQ(strlen(text), sizeof(text) - 1);
  EXPECT_EQ(strncmp(text, "A very long spe", sizeof(text) - 1), 0);
}

struct CodCase {
  uint32_t cod;
  bool audio;
};

class AudioDeviceTest : public ::testing::TestWithParam<CodCase> {};

TEST_P(AudioDeviceTest, MajorClass) { EXPECT_EQ(is_audio_device(GetParam().cod), GetParam().audio); }

// Loudspeaker, headphones, car audio, smartphone (with the audio service bit), computer, printer (rendering bit)
INSTANTIATE_TEST_SUITE_P(UsbBluetooth, AudioDeviceTest,
                         ::testing::Values(CodCase{0x240414, true}, CodCase{0x240418, true}, CodCase{0x200420, true},
                                           CodCase{0x5A020C, false}, CodCase{0x10010C, false},
                                           CodCase{0x040680, false}));

static const uint8_t OTHER_ADDRESS[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

TEST(AddFoundSpeaker, AddsAndFillsInTheName) {
  FoundSpeaker list[2]{};
  uint8_t count = 0;
  EXPECT_TRUE(add_found_speaker(list, count, 2, SPEAKER_ADDRESS, ""));
  EXPECT_EQ(count, 1);
  EXPECT_STREQ(list[0].name, "");
  // The name comes in a later result for the same device
  EXPECT_TRUE(add_found_speaker(list, count, 2, SPEAKER_ADDRESS, "SBL 4 A2"));
  EXPECT_EQ(count, 1);
  EXPECT_STREQ(list[0].name, "SBL 4 A2");
  // A repeat, or a result without a name, changes nothing
  EXPECT_FALSE(add_found_speaker(list, count, 2, SPEAKER_ADDRESS, "SBL 4 A2"));
  EXPECT_FALSE(add_found_speaker(list, count, 2, SPEAKER_ADDRESS, ""));
  EXPECT_STREQ(list[0].name, "SBL 4 A2");
}

TEST(AddFoundSpeaker, DropsDevicesPastTheMaximum) {
  FoundSpeaker list[1]{};
  uint8_t count = 0;
  EXPECT_TRUE(add_found_speaker(list, count, 1, SPEAKER_ADDRESS, "SBL 4 A2"));
  EXPECT_FALSE(add_found_speaker(list, count, 1, OTHER_ADDRESS, "Radio"));
  EXPECT_EQ(count, 1);
  EXPECT_EQ(memcmp(list[0].address, SPEAKER_ADDRESS, 6), 0);
}

TEST(AddFoundSpeaker, LongNameIsCut) {
  FoundSpeaker list[1]{};
  uint8_t count = 0;
  add_found_speaker(list, count, 1, SPEAKER_ADDRESS, "A speaker name that is longer than thirty-one characters");
  EXPECT_EQ(strlen(list[0].name), sizeof(list[0].name) - 1);
}

struct ButtonCase {
  uint8_t key_code;
  SpeakerButton button;
};

class SpeakerButtonTest : public ::testing::TestWithParam<ButtonCase> {};

TEST_P(SpeakerButtonTest, KeyCode) { EXPECT_EQ(speaker_button(GetParam().key_code), GetParam().button); }

INSTANTIATE_TEST_SUITE_P(UsbBluetooth, SpeakerButtonTest,
                         ::testing::Values(ButtonCase{0x44, SpeakerButton::SPEAKER_BUTTON_PLAY},
                                           ButtonCase{0x46, SpeakerButton::SPEAKER_BUTTON_PAUSE},
                                           ButtonCase{0x45, SpeakerButton::SPEAKER_BUTTON_STOP},
                                           ButtonCase{0x4B, SpeakerButton::SPEAKER_BUTTON_NEXT},
                                           ButtonCase{0x4C, SpeakerButton::SPEAKER_BUTTON_PREVIOUS},
                                           ButtonCase{0x41, SpeakerButton::SPEAKER_BUTTON_VOLUME_UP},
                                           ButtonCase{0x42, SpeakerButton::SPEAKER_BUTTON_VOLUME_DOWN},
                                           ButtonCase{0x40, SpeakerButton::SPEAKER_BUTTON_NONE},
                                           ButtonCase{0x00, SpeakerButton::SPEAKER_BUTTON_NONE}));

TEST(SpeakerButtonNames, OneNamePerButton) {
  EXPECT_EQ(sizeof(SPEAKER_BUTTON_NAMES) / sizeof(SPEAKER_BUTTON_NAMES[0]),
            static_cast<size_t>(SpeakerButton::SPEAKER_BUTTON_NONE));
  EXPECT_STREQ(SPEAKER_BUTTON_NAMES[static_cast<uint8_t>(SpeakerButton::SPEAKER_BUTTON_NEXT)], "next");
}

TEST(AvrcLevel, RoundTrip) {
  EXPECT_FLOAT_EQ(avrc_level(0), 0.0f);
  EXPECT_FLOAT_EQ(avrc_level(127), 1.0f);
  EXPECT_FLOAT_EQ(avrc_level(200), 1.0f);
  // A level set from the speaker's volume gives that volume back, so it is not sent back as a change
  for (unsigned volume = 0; volume <= 127; volume++)
    EXPECT_EQ(avrc_volume(avrc_level(volume)), volume);
}

static const uint8_t RADIO_ADDRESS[6] = {0xE0, 0x17, 0xDE, 0xD3, 0x59, 0x79};

TEST(AddFoundSpeaker, StrongestSignalFirst) {
  FoundSpeaker list[3]{};
  uint8_t count = 0;
  EXPECT_TRUE(add_found_speaker(list, count, 3, SPEAKER_ADDRESS, "SBL 4 A2", -80));
  EXPECT_TRUE(add_found_speaker(list, count, 3, RADIO_ADDRESS, "TCL-212BT", -45));
  EXPECT_TRUE(add_found_speaker(list, count, 3, OTHER_ADDRESS, "", RSSI_UNKNOWN));
  ASSERT_EQ(count, 3);
  EXPECT_EQ(memcmp(list[0].address, RADIO_ADDRESS, 6), 0);
  EXPECT_EQ(memcmp(list[1].address, SPEAKER_ADDRESS, 6), 0);
  EXPECT_EQ(memcmp(list[2].address, OTHER_ADDRESS, 6), 0);
}

TEST(AddFoundSpeaker, KeepsTheFirstSignalStrength) {
  FoundSpeaker list[2]{};
  uint8_t count = 0;
  add_found_speaker(list, count, 2, SPEAKER_ADDRESS, "SBL 4 A2", -80);
  add_found_speaker(list, count, 2, RADIO_ADDRESS, "TCL-212BT", -60);
  // A later answer of the same device does not reorder the list during the search
  EXPECT_FALSE(add_found_speaker(list, count, 2, SPEAKER_ADDRESS, "SBL 4 A2", -40));
  EXPECT_EQ(memcmp(list[0].address, RADIO_ADDRESS, 6), 0);
  EXPECT_EQ(list[1].rssi, -80);
  // A device first seen without a signal strength gets its place once one arrives
  FoundSpeaker other[2]{};
  uint8_t other_count = 0;
  add_found_speaker(other, other_count, 2, SPEAKER_ADDRESS, "SBL 4 A2");
  add_found_speaker(other, other_count, 2, RADIO_ADDRESS, "TCL-212BT", -60);
  EXPECT_TRUE(add_found_speaker(other, other_count, 2, SPEAKER_ADDRESS, "", -30));
  EXPECT_EQ(memcmp(other[0].address, SPEAKER_ADDRESS, 6), 0);
}

TEST(AddFoundSpeaker, FullListGivesTheWeakestPlaceToAStrongerDevice) {
  FoundSpeaker list[2]{};
  uint8_t count = 0;
  add_found_speaker(list, count, 2, SPEAKER_ADDRESS, "SBL 4 A2", -50);
  add_found_speaker(list, count, 2, OTHER_ADDRESS, "Far away", -90);
  EXPECT_TRUE(add_found_speaker(list, count, 2, RADIO_ADDRESS, "TCL-212BT", -70));
  ASSERT_EQ(count, 2);
  EXPECT_EQ(memcmp(list[0].address, SPEAKER_ADDRESS, 6), 0);
  EXPECT_EQ(memcmp(list[1].address, RADIO_ADDRESS, 6), 0);
  // A weaker device than all of them is left out
  EXPECT_FALSE(add_found_speaker(list, count, 2, OTHER_ADDRESS, "Far away", -95));
}

TEST(NextUnnamedSpeaker, EachDeviceWithoutANameIsAskedOnce) {
  FoundSpeaker list[3]{};
  uint8_t count = 0;
  add_found_speaker(list, count, 3, SPEAKER_ADDRESS, "SBL 4 A2", -50);
  add_found_speaker(list, count, 3, RADIO_ADDRESS, "", -60);
  add_found_speaker(list, count, 3, OTHER_ADDRESS, "", -70);
  FoundSpeaker *next = next_unnamed_speaker(list, count);
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(memcmp(next->address, RADIO_ADDRESS, 6), 0);
  next->name_requested = true;
  // The radio answers with its name
  EXPECT_TRUE(add_found_speaker(list, count, 3, RADIO_ADDRESS, "TCL-212BT"));
  next = next_unnamed_speaker(list, count);
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(memcmp(next->address, OTHER_ADDRESS, 6), 0);
  // This one does not answer; it is not asked again
  next->name_requested = true;
  EXPECT_EQ(next_unnamed_speaker(list, count), nullptr);
}

}  // namespace esphome::usb_bluetooth::testing
