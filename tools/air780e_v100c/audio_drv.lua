-- SPDX-FileCopyrightText: 2026 ZQYuan
-- SPDX-License-Identifier: Apache-2.0

local exaudio = require("exaudio")

-- Air780EHV 13/113 V2048+ new-audio configuration used by the V100C voice path.
local AUDIO_CONFIG = {
    model = "es8311",
    i2c_id = 0,
    pa_ctrl = gpio.AUDIOPA_EN,
    dac_ctrl = 20,
    dac_delay = 6,
    pa_delay = 100,
    dac_time_delay = 100,
    bits_per_sample = 16,
    pa_on_level = 1,
    audio_mode = "new",
}

local function init_audio_device()
    if not exaudio.setup(AUDIO_CONFIG) then
        return false
    end
    exaudio.vol(70)
    exaudio.mic_vol(65)
    return true
end

local function get_multimedia_id()
    return 0
end

return {
    initAudioDevice = init_audio_device,
    getMultimediaId = get_multimedia_id,
}
