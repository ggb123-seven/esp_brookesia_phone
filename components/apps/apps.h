#pragma once

#include "sdkconfig.h"

// #include "smart_gadget/SmartGadget.hpp"

#if CONFIG_EXAMPLE_ENABLE_APP_MUSIC_PLAYER
#include "music_player/MusicPlayer.hpp"
#endif

#include "setting/Setting.hpp"

#if CONFIG_EXAMPLE_ENABLE_APP_GAME_2048
#include "game_2048/Game_2048.hpp"
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_CALCULATOR
#include "calculator/Calculator.hpp"
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_FINGERPRINT
#include "fingerprint/FingerprintApp.hpp"
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_ENVIRONMENT_MONITOR
#include "environment_monitor/EnvironmentMonitorApp.hpp"
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_CLASSROOM_SCHEDULE
#include "classroom_schedule/ClassroomScheduleApp.hpp"
#endif

#include "camera/Camera.hpp"

#if CONFIG_EXAMPLE_ENABLE_APP_VIDEO_PLAYER
#include "video_player/VideoPlayer.hpp"
#endif
