#pragma once

#include "sdkconfig.h"

// #include "smart_gadget/SmartGadget.hpp"
#include "music_player/MusicPlayer.hpp"
#include "setting/Setting.hpp"

#if CONFIG_EXAMPLE_ENABLE_APP_GAME_2048
#include "game_2048/Game_2048.hpp"
#endif

#if CONFIG_EXAMPLE_ENABLE_APP_CALCULATOR
#include "calculator/Calculator.hpp"
#endif

#include "camera/Camera.hpp"
#include "video_player/VideoPlayer.hpp"
