#pragma once

#include <common.h>

// APU初期化
void apu_init();

// T-cycle単位でAPUを進める
void apu_tick();

// レジスタアクセス
u8 apu_read(u16 address);
void apu_write(u16 address, u8 value);

// SDL2オーディオ初期化（ui_initから呼び出し）
void apu_audio_init();
void apu_audio_shutdown();
