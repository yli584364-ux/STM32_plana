#include "bluetooth.h"

BluetoothA2DPSource a2dpSource;

static const int a2dpSampleRate = 44100;    // 44.1kHz 立体声 16bit
static const float a2dpToneFreq = 440.0f;   // 440Hz 正弦波
static float a2dpPhase = 0.0f;

// 回调：生成要发送给蓝牙设备的 PCM 音频数据
int32_t get_sound_data(uint8_t *data, int32_t byteCount)
{
  int16_t *samples = (int16_t *)data;
  int32_t sampleCount = byteCount / 2; // 16bit = 2 字节

  float phaseInc = 2.0f * PI * a2dpToneFreq / (float)a2dpSampleRate;

  for (int32_t i = 0; i < sampleCount; i += 2)
  {
    // 生成一个比较小的幅度，避免过载
    int16_t v = (int16_t)(sin(a2dpPhase) * 8000.0f);

    // 单声道：左声道有信号，右声道静音
    samples[i] = v;     // Left
    samples[i + 1] = v; // Right

    a2dpPhase += phaseInc;
    if (a2dpPhase > 2.0f * PI)
    {
      a2dpPhase -= 2.0f * PI;
    }
  }

  return byteCount; // 告诉库我们填充了多少字节
}