#include <apu.h>
#include <SDL2/SDL.h>

// ============================================================================
// チャンネル共通構造体
// ============================================================================
typedef struct {
    bool enabled;           // チャンネル有効フラグ
    bool dac_enabled;       // DAC有効フラグ
    
    // 長さカウンター
    u16 length_counter;
    bool length_enabled;
    
    // エンベロープ
    u8 volume;
    u8 envelope_initial;
    bool envelope_direction; // true=増加, false=減少
    u8 envelope_period;
    u8 envelope_timer;
    
    // 周波数/タイマー
    u16 frequency;
    u16 timer;
    
    // 出力
    u8 output;
} channel_common_t;

// ============================================================================
// チャンネル1構造体（スイープ付き矩形波）
// ============================================================================
typedef struct {
    channel_common_t common;
    
    // デューティサイクル
    u8 duty;                // 0-3 (12.5%, 25%, 50%, 75%)
    u8 duty_position;       // 0-7
    
    // スイープ
    u8 sweep_period;
    bool sweep_direction;   // true=減少, false=増加
    u8 sweep_shift;
    u8 sweep_timer;
    u16 sweep_shadow;
    bool sweep_enabled;
} channel1_t;

// ============================================================================
// チャンネル2構造体（矩形波）
// ============================================================================
typedef struct {
    channel_common_t common;
    
    // デューティサイクル
    u8 duty;
    u8 duty_position;
} channel2_t;

// ============================================================================
// チャンネル3構造体（波形メモリ）
// ============================================================================
typedef struct {
    channel_common_t common;
    
    // 波形
    u8 wave_ram[16];        // 32サンプル（4bit x 2 per byte）
    u8 wave_position;       // 0-31
    u8 volume_shift;        // 0=mute, 1=100%, 2=50%, 3=25%
} channel3_t;

// ============================================================================
// チャンネル4構造体（ノイズ）
// ============================================================================
typedef struct {
    channel_common_t common;
    
    // LFSR
    u16 lfsr;               // 15-bit LFSR
    bool width_mode;        // true=7-bit, false=15-bit
    u8 clock_shift;
    u8 divisor_code;
} channel4_t;

// ============================================================================
// APUコンテキスト構造体
// ============================================================================
typedef struct {
    // チャンネル
    channel1_t ch1;
    channel2_t ch2;
    channel3_t ch3;
    channel4_t ch4;
    
    // フレームシーケンサー
    u16 frame_sequencer_timer;  // 8192でリセット
    u8 frame_sequencer_step;    // 0-7
    
    // マスターコントロール
    bool enabled;               // NR52 bit 7
    u8 nr50;                    // マスターボリューム
    u8 nr51;                    // パンニング
    
    // オーディオ出力
    u32 sample_timer;           // ダウンサンプリング用
    int16_t *audio_buffer;
    u32 buffer_position;
    u32 buffer_size;
} apu_context;

// ============================================================================
// オーディオ定数
// ============================================================================
#define APU_SAMPLE_RATE 44100
#define APU_BUFFER_SIZE 4096

// ============================================================================
// ダウンサンプリング定数
// Game Boy CPUは4.194304 MHzで動作
// 44100 Hzにダウンサンプリングするため、約95 T-cycleごとにサンプルを生成
// 4194304 / 44100 ≈ 95.1
// Requirements: 10.6
// ============================================================================
#define CPU_CLOCK_RATE 4194304
#define SAMPLE_PERIOD (CPU_CLOCK_RATE / APU_SAMPLE_RATE)

// ============================================================================
// 静的コンテキスト変数
// ============================================================================
static apu_context ctx = {0};

// ============================================================================
// SDL2オーディオデバイスID
// ============================================================================
static SDL_AudioDeviceID audio_device_id = 0;

// ============================================================================
// レジスタ値を保存する配列（0xFF10-0xFF26の23バイト）
// ============================================================================
static u8 registers[23] = {0};

// ============================================================================
// レジスタ読み取りORマスク
// 書き込み専用ビットは読み取り時に1を返す
// ============================================================================
static const u8 read_masks[] = {
    0x80, // NR10 (0xFF10)
    0x3F, // NR11 (0xFF11)
    0x00, // NR12 (0xFF12)
    0xFF, // NR13 (0xFF13)
    0xBF, // NR14 (0xFF14)
    0xFF, // NR15 (0xFF15, unused)
    0x3F, // NR21 (0xFF16)
    0x00, // NR22 (0xFF17)
    0xFF, // NR23 (0xFF18)
    0xBF, // NR24 (0xFF19)
    0x7F, // NR30 (0xFF1A)
    0xFF, // NR31 (0xFF1B)
    0x9F, // NR32 (0xFF1C)
    0xFF, // NR33 (0xFF1D)
    0xBF, // NR34 (0xFF1E)
    0xFF, // NR35 (0xFF1F, unused)
    0xFF, // NR41 (0xFF20)
    0x00, // NR42 (0xFF21)
    0x00, // NR43 (0xFF22)
    0xBF, // NR44 (0xFF23)
    0x00, // NR50 (0xFF24)
    0x00, // NR51 (0xFF25)
    0x70, // NR52 (0xFF26)
};

// ============================================================================
// デューティサイクル波形テーブル
// 各デューティサイクル（12.5%, 25%, 50%, 75%）の8ステップ波形パターン
// Requirements: 3.7
// ============================================================================
static const u8 duty_table[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1}, // 25%
    {1, 0, 0, 0, 0, 1, 1, 1}, // 50%
    {0, 1, 1, 1, 1, 1, 1, 0}, // 75%
};

// ============================================================================
// ノイズ除数テーブル
// チャンネル4のLFSRクロック周期を決定するための除数
// divisor_code (0-7) からタイマー周期の基本値を取得
// Requirements: 6.6
// ============================================================================
static const u8 divisor_table[] = {8, 16, 32, 48, 64, 80, 96, 112};

// ============================================================================
// LFSRクロック処理
// ノイズ生成のためのLinear Feedback Shift Register処理
// Requirements: 6.6, 6.7
// 
// アルゴリズム:
// 1. ビット0とビット1をXOR
// 2. LFSRを右に1ビットシフト
// 3. XOR結果をビット14にセット
// 4. 7ビットモードの場合、XOR結果をビット6にもセット
// ============================================================================
static void clock_lfsr() {
    // ビット0とビット1をXOR
    u8 xor_result = (ctx.ch4.lfsr & 0x01) ^ ((ctx.ch4.lfsr >> 1) & 0x01);
    
    // LFSRを右に1ビットシフト
    ctx.ch4.lfsr >>= 1;
    
    // XOR結果をビット14にセット
    ctx.ch4.lfsr |= (xor_result << 14);
    
    // 7ビットモードの場合、ビット6にもXOR結果をセット
    // Requirements: 6.7
    if (ctx.ch4.width_mode) {
        // まずビット6をクリアしてからセット
        ctx.ch4.lfsr &= ~(1 << 6);
        ctx.ch4.lfsr |= (xor_result << 6);
    }
}

// ============================================================================
// チャンネル1トリガー処理
// Requirements: 3.5, 7.1, 8.1
// ============================================================================
static void trigger_channel1() {
    // DACが有効な場合のみチャンネルを有効化
    // DAC有効条件: NR12の上位5ビット（ボリューム+方向）が0でない
    if (ctx.ch1.common.dac_enabled) {
        ctx.ch1.common.enabled = true;
    }
    
    // 長さカウンターが0の場合、64にリロード
    if (ctx.ch1.common.length_counter == 0) {
        ctx.ch1.common.length_counter = 64;
    }
    
    // エンベロープをリロード
    // Requirements: 7.1
    ctx.ch1.common.volume = ctx.ch1.common.envelope_initial;
    ctx.ch1.common.envelope_timer = ctx.ch1.common.envelope_period;
    
    // 周波数をスイープシャドウにコピー
    ctx.ch1.sweep_shadow = ctx.ch1.common.frequency;
    
    // スイープタイマーをリロード（周期が0の場合は8を使用）
    ctx.ch1.sweep_timer = ctx.ch1.sweep_period ? ctx.ch1.sweep_period : 8;
    
    // スイープ有効フラグを設定（周期または シフトが非ゼロの場合）
    ctx.ch1.sweep_enabled = (ctx.ch1.sweep_period > 0) || (ctx.ch1.sweep_shift > 0);
    
    // シフトが非ゼロの場合、初期オーバーフローチェックを実行
    // Requirements: 3.9
    if (ctx.ch1.sweep_shift > 0) {
        u16 new_freq;
        u16 shift_amount = ctx.ch1.sweep_shadow >> ctx.ch1.sweep_shift;
        
        if (ctx.ch1.sweep_direction) {
            // 減少モード
            new_freq = ctx.ch1.sweep_shadow - shift_amount;
        } else {
            // 増加モード
            new_freq = ctx.ch1.sweep_shadow + shift_amount;
        }
        
        // オーバーフローチェック
        if (new_freq > 2047) {
            ctx.ch1.common.enabled = false;
        }
    }
}

// ============================================================================
// チャンネル2トリガー処理
// Requirements: 4.4, 7.1
// ============================================================================
static void trigger_channel2() {
    // DACが有効な場合のみチャンネルを有効化
    // DAC有効条件: NR22の上位5ビット（ボリューム+方向）が0でない
    if (ctx.ch2.common.dac_enabled) {
        ctx.ch2.common.enabled = true;
    }
    
    // 長さカウンターが0の場合、64にリロード
    if (ctx.ch2.common.length_counter == 0) {
        ctx.ch2.common.length_counter = 64;
    }
    
    // エンベロープをリロード
    // Requirements: 7.1
    ctx.ch2.common.volume = ctx.ch2.common.envelope_initial;
    ctx.ch2.common.envelope_timer = ctx.ch2.common.envelope_period;
}

// ============================================================================
// チャンネル3トリガー処理
// Requirements: 5.5
// ============================================================================
static void trigger_channel3() {
    // DACが有効な場合のみチャンネルを有効化
    // DAC有効条件: NR30のビット7が1
    if (ctx.ch3.common.dac_enabled) {
        ctx.ch3.common.enabled = true;
    }
    
    // 長さカウンターが0の場合、256にリロード
    // チャンネル3の長さカウンターは256が最大（チャンネル1,2は64）
    if (ctx.ch3.common.length_counter == 0) {
        ctx.ch3.common.length_counter = 256;
    }
    
    // 波形位置をリセット
    ctx.ch3.wave_position = 0;
}

// ============================================================================
// チャンネル1レジスタ書き込み処理
// Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6
// ============================================================================
static void write_channel1(u16 address, u8 value) {
    switch (address) {
        case 0xFF10: // NR10 - スイープレジスタ
            // Requirements: 3.1
            // ビット6-4: スイープ周期
            ctx.ch1.sweep_period = (value >> 4) & 0x07;
            // ビット3: スイープ方向 (0=増加, 1=減少)
            ctx.ch1.sweep_direction = (value & 0x08) != 0;
            // ビット2-0: スイープシフト
            ctx.ch1.sweep_shift = value & 0x07;
            break;
            
        case 0xFF11: // NR11 - デューティ/長さレジスタ
            // Requirements: 3.2
            // ビット7-6: デューティサイクル
            ctx.ch1.duty = (value >> 6) & 0x03;
            // ビット5-0: 長さロード (64 - value)
            ctx.ch1.common.length_counter = 64 - (value & 0x3F);
            break;
            
        case 0xFF12: // NR12 - エンベロープレジスタ
            // Requirements: 3.3
            // ビット7-4: 初期ボリューム
            ctx.ch1.common.envelope_initial = (value >> 4) & 0x0F;
            // ビット3: エンベロープ方向 (0=減少, 1=増加)
            ctx.ch1.common.envelope_direction = (value & 0x08) != 0;
            // ビット2-0: エンベロープ周期
            ctx.ch1.common.envelope_period = value & 0x07;
            // DAC有効: 上位5ビット（ボリューム+方向）が0でない場合
            ctx.ch1.common.dac_enabled = (value & 0xF8) != 0;
            // DACが無効になった場合、チャンネルも無効化
            if (!ctx.ch1.common.dac_enabled) {
                ctx.ch1.common.enabled = false;
            }
            break;
            
        case 0xFF13: // NR13 - 周波数下位レジスタ
            // Requirements: 3.4
            // 周波数の下位8ビットを設定
            ctx.ch1.common.frequency = (ctx.ch1.common.frequency & 0x700) | value;
            break;
            
        case 0xFF14: // NR14 - トリガー/周波数上位レジスタ
            // Requirements: 3.5, 3.6
            // ビット2-0: 周波数の上位3ビット
            ctx.ch1.common.frequency = (ctx.ch1.common.frequency & 0x0FF) | ((value & 0x07) << 8);
            // ビット6: 長さ有効フラグ
            ctx.ch1.common.length_enabled = (value & 0x40) != 0;
            // ビット7: トリガー
            if (value & 0x80) {
                trigger_channel1();
            }
            break;
    }
}

// ============================================================================
// チャンネル2レジスタ書き込み処理
// Requirements: 4.1, 4.2, 4.3, 4.4, 4.5
// ============================================================================
static void write_channel2(u16 address, u8 value) {
    switch (address) {
        case 0xFF16: // NR21 - デューティ/長さレジスタ
            // Requirements: 4.1
            // ビット7-6: デューティサイクル
            ctx.ch2.duty = (value >> 6) & 0x03;
            // ビット5-0: 長さロード (64 - value)
            ctx.ch2.common.length_counter = 64 - (value & 0x3F);
            break;
            
        case 0xFF17: // NR22 - エンベロープレジスタ
            // Requirements: 4.2
            // ビット7-4: 初期ボリューム
            ctx.ch2.common.envelope_initial = (value >> 4) & 0x0F;
            // ビット3: エンベロープ方向 (0=減少, 1=増加)
            ctx.ch2.common.envelope_direction = (value & 0x08) != 0;
            // ビット2-0: エンベロープ周期
            ctx.ch2.common.envelope_period = value & 0x07;
            // DAC有効: 上位5ビット（ボリューム+方向）が0でない場合
            ctx.ch2.common.dac_enabled = (value & 0xF8) != 0;
            // DACが無効になった場合、チャンネルも無効化
            if (!ctx.ch2.common.dac_enabled) {
                ctx.ch2.common.enabled = false;
            }
            break;
            
        case 0xFF18: // NR23 - 周波数下位レジスタ
            // Requirements: 4.3
            // 周波数の下位8ビットを設定
            ctx.ch2.common.frequency = (ctx.ch2.common.frequency & 0x700) | value;
            break;
            
        case 0xFF19: // NR24 - トリガー/周波数上位レジスタ
            // Requirements: 4.4, 4.5
            // ビット2-0: 周波数の上位3ビット
            ctx.ch2.common.frequency = (ctx.ch2.common.frequency & 0x0FF) | ((value & 0x07) << 8);
            // ビット6: 長さ有効フラグ
            ctx.ch2.common.length_enabled = (value & 0x40) != 0;
            // ビット7: トリガー
            if (value & 0x80) {
                trigger_channel2();
            }
            break;
    }
}

// ============================================================================
// チャンネル4トリガー処理
// Requirements: 6.4, 7.1
// ============================================================================
static void trigger_channel4() {
    // DACが有効な場合のみチャンネルを有効化
    // DAC有効条件: NR42の上位5ビット（ボリューム+方向）が0でない
    if (ctx.ch4.common.dac_enabled) {
        ctx.ch4.common.enabled = true;
    }
    
    // 長さカウンターが0の場合、64にリロード
    if (ctx.ch4.common.length_counter == 0) {
        ctx.ch4.common.length_counter = 64;
    }
    
    // エンベロープをリロード
    // Requirements: 7.1
    ctx.ch4.common.volume = ctx.ch4.common.envelope_initial;
    ctx.ch4.common.envelope_timer = ctx.ch4.common.envelope_period;
    
    // LFSRを全1にリセット（15ビット: 0x7FFF）
    // Requirements: 6.4
    ctx.ch4.lfsr = 0x7FFF;
}

// ============================================================================
// チャンネル4レジスタ書き込み処理
// Requirements: 6.1, 6.2, 6.3, 6.4, 6.5
// ============================================================================
static void write_channel4(u16 address, u8 value) {
    switch (address) {
        case 0xFF20: // NR41 - 長さレジスタ
            // Requirements: 6.1
            // ビット5-0: 長さロード (64 - value)
            ctx.ch4.common.length_counter = 64 - (value & 0x3F);
            break;
            
        case 0xFF21: // NR42 - エンベロープレジスタ
            // Requirements: 6.2
            // ビット7-4: 初期ボリューム
            ctx.ch4.common.envelope_initial = (value >> 4) & 0x0F;
            // ビット3: エンベロープ方向 (0=減少, 1=増加)
            ctx.ch4.common.envelope_direction = (value & 0x08) != 0;
            // ビット2-0: エンベロープ周期
            ctx.ch4.common.envelope_period = value & 0x07;
            // DAC有効: 上位5ビット（ボリューム+方向）が0でない場合
            ctx.ch4.common.dac_enabled = (value & 0xF8) != 0;
            // DACが無効になった場合、チャンネルも無効化
            if (!ctx.ch4.common.dac_enabled) {
                ctx.ch4.common.enabled = false;
            }
            break;
            
        case 0xFF22: // NR43 - ポリノミアルレジスタ
            // Requirements: 6.3
            // ビット7-4: クロックシフト
            ctx.ch4.clock_shift = (value >> 4) & 0x0F;
            // ビット3: 幅モード (0=15ビット, 1=7ビット)
            ctx.ch4.width_mode = (value & 0x08) != 0;
            // ビット2-0: 除数コード
            ctx.ch4.divisor_code = value & 0x07;
            break;
            
        case 0xFF23: // NR44 - トリガー/長さ有効レジスタ
            // Requirements: 6.4, 6.5
            // ビット6: 長さ有効フラグ
            ctx.ch4.common.length_enabled = (value & 0x40) != 0;
            // ビット7: トリガー
            if (value & 0x80) {
                trigger_channel4();
            }
            break;
    }
}

// ============================================================================
// チャンネル3レジスタ書き込み処理
// Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6
// ============================================================================
static void write_channel3(u16 address, u8 value) {
    switch (address) {
        case 0xFF1A: // NR30 - DAC有効レジスタ
            // Requirements: 5.1
            // ビット7: DAC有効フラグ
            ctx.ch3.common.dac_enabled = (value & 0x80) != 0;
            // DACが無効になった場合、チャンネルも無効化
            if (!ctx.ch3.common.dac_enabled) {
                ctx.ch3.common.enabled = false;
            }
            break;
            
        case 0xFF1B: // NR31 - 長さレジスタ
            // Requirements: 5.2
            // ビット7-0: 長さロード (256 - value)
            // チャンネル3は8ビット全体を使用（0-255 -> 256-1）
            ctx.ch3.common.length_counter = 256 - value;
            break;
            
        case 0xFF1C: // NR32 - ボリュームレジスタ
            // Requirements: 5.3
            // ビット6-5: ボリュームコード
            // 0=mute(shift 4), 1=100%(shift 0), 2=50%(shift 1), 3=25%(shift 2)
            {
                u8 volume_code = (value >> 5) & 0x03;
                // ボリュームコードからシフト量への変換
                // code 0 -> shift 4 (mute: 右シフト4で0になる)
                // code 1 -> shift 0 (100%: シフトなし)
                // code 2 -> shift 1 (50%: 右シフト1)
                // code 3 -> shift 2 (25%: 右シフト2)
                static const u8 volume_shift_table[4] = {4, 0, 1, 2};
                ctx.ch3.volume_shift = volume_shift_table[volume_code];
            }
            break;
            
        case 0xFF1D: // NR33 - 周波数下位レジスタ
            // Requirements: 5.4
            // 周波数の下位8ビットを設定
            ctx.ch3.common.frequency = (ctx.ch3.common.frequency & 0x700) | value;
            break;
            
        case 0xFF1E: // NR34 - トリガー/周波数上位レジスタ
            // Requirements: 5.5, 5.6
            // ビット2-0: 周波数の上位3ビット
            ctx.ch3.common.frequency = (ctx.ch3.common.frequency & 0x0FF) | ((value & 0x07) << 8);
            // ビット6: 長さ有効フラグ
            ctx.ch3.common.length_enabled = (value & 0x40) != 0;
            // ビット7: トリガー
            if (value & 0x80) {
                trigger_channel3();
            }
            break;
    }
}

// ============================================================================
// APU初期化
// ============================================================================
void apu_init() {
    // TODO: 全レジスタをデフォルト値に初期化
}

// ============================================================================
// 長さカウンターのクロック処理
// 各チャンネルの長さカウンターをデクリメントし、0に達したらチャンネルを無効化
// Requirements: 2.2, 8.2, 8.3, 8.4
// ============================================================================
static void clock_length_counters() {
    // チャンネル1の長さカウンター
    if (ctx.ch1.common.length_enabled && ctx.ch1.common.length_counter > 0) {
        ctx.ch1.common.length_counter--;
        if (ctx.ch1.common.length_counter == 0) {
            ctx.ch1.common.enabled = false;
        }
    }
    
    // チャンネル2の長さカウンター
    if (ctx.ch2.common.length_enabled && ctx.ch2.common.length_counter > 0) {
        ctx.ch2.common.length_counter--;
        if (ctx.ch2.common.length_counter == 0) {
            ctx.ch2.common.enabled = false;
        }
    }
    
    // チャンネル3の長さカウンター
    if (ctx.ch3.common.length_enabled && ctx.ch3.common.length_counter > 0) {
        ctx.ch3.common.length_counter--;
        if (ctx.ch3.common.length_counter == 0) {
            ctx.ch3.common.enabled = false;
        }
    }
    
    // チャンネル4の長さカウンター
    if (ctx.ch4.common.length_enabled && ctx.ch4.common.length_counter > 0) {
        ctx.ch4.common.length_counter--;
        if (ctx.ch4.common.length_counter == 0) {
            ctx.ch4.common.enabled = false;
        }
    }
}

// ============================================================================
// エンベロープユニットのクロック処理（単一チャンネル）
// Requirements: 7.2, 7.3, 7.4, 7.5
// ============================================================================
static void clock_envelope(channel_common_t *ch) {
    // 周期が0の場合は処理しない
    // Requirements: 7.5
    if (ch->envelope_period == 0) {
        return;
    }
    
    // エンベロープタイマーをデクリメント
    if (ch->envelope_timer > 0) {
        ch->envelope_timer--;
    }
    
    // タイマーが0に達したら
    if (ch->envelope_timer == 0) {
        // タイマーをリロード
        ch->envelope_timer = ch->envelope_period;
        
        // ボリュームを増減
        // Requirements: 7.2, 7.3, 7.4
        if (ch->envelope_direction) {
            // 増加モード: ボリュームが15未満なら増加
            if (ch->volume < 15) {
                ch->volume++;
            }
        } else {
            // 減少モード: ボリュームが0より大きければ減少
            if (ch->volume > 0) {
                ch->volume--;
            }
        }
    }
}

// ============================================================================
// 全チャンネルのエンベロープをクロック
// チャンネル1, 2, 4のみ（チャンネル3はエンベロープを持たない）
// Requirements: 2.4
// ============================================================================
static void clock_envelopes() {
    clock_envelope(&ctx.ch1.common);
    clock_envelope(&ctx.ch2.common);
    clock_envelope(&ctx.ch4.common);
}

// ============================================================================
// スイープユニットのクロック処理
// チャンネル1の周波数スイープを処理
// Requirements: 2.3, 3.8, 3.9
// ============================================================================
static void clock_sweep() {
    // スイープタイマーが0より大きい場合、デクリメント
    if (ctx.ch1.sweep_timer > 0) {
        ctx.ch1.sweep_timer--;
    }
    
    // タイマーが0に達し、スイープが有効で、周期が0より大きい場合
    if (ctx.ch1.sweep_timer == 0 && ctx.ch1.sweep_enabled && ctx.ch1.sweep_period > 0) {
        // タイマーをリロード
        ctx.ch1.sweep_timer = ctx.ch1.sweep_period;
        
        // 新しい周波数を計算
        u16 new_freq;
        u16 shift_amount = ctx.ch1.sweep_shadow >> ctx.ch1.sweep_shift;
        
        if (ctx.ch1.sweep_direction) {
            // 減少モード: 周波数を減らす
            new_freq = ctx.ch1.sweep_shadow - shift_amount;
        } else {
            // 増加モード: 周波数を増やす
            new_freq = ctx.ch1.sweep_shadow + shift_amount;
        }
        
        // オーバーフローチェック: 2047を超えたらチャンネルを無効化
        if (new_freq > 2047) {
            ctx.ch1.common.enabled = false;
        } else {
            // シフト量が0より大きい場合のみ、周波数とシャドウを更新
            if (ctx.ch1.sweep_shift > 0) {
                ctx.ch1.sweep_shadow = new_freq;
                ctx.ch1.common.frequency = new_freq;
                
                // 更新後に再度オーバーフローチェック（次の計算をシミュレート）
                u16 next_freq;
                u16 next_shift_amount = new_freq >> ctx.ch1.sweep_shift;
                
                if (ctx.ch1.sweep_direction) {
                    next_freq = new_freq - next_shift_amount;
                } else {
                    next_freq = new_freq + next_shift_amount;
                }
                
                if (next_freq > 2047) {
                    ctx.ch1.common.enabled = false;
                }
            }
        }
    }
}

// ============================================================================
// チャンネル1のティック処理（サンプル生成）
// タイマーをデクリメントし、0に達したらデューティ位置を進める
// 出力はデューティテーブルとボリュームに基づいて計算
// Requirements: 3.7
// ============================================================================
static void tick_channel1() {
    // チャンネルが無効の場合は出力を0にして終了
    if (!ctx.ch1.common.enabled) {
        ctx.ch1.common.output = 0;
        return;
    }
    
    // タイマーをデクリメント
    if (ctx.ch1.common.timer > 0) {
        ctx.ch1.common.timer--;
    }
    
    // タイマーが0に達したら
    if (ctx.ch1.common.timer == 0) {
        // タイマーをリロード: (2048 - frequency) * 4
        ctx.ch1.common.timer = (2048 - ctx.ch1.common.frequency) * 4;
        
        // デューティ位置を進める（0-7で循環）
        ctx.ch1.duty_position = (ctx.ch1.duty_position + 1) & 0x07;
    }
    
    // 出力を計算: デューティテーブル値 * ボリューム
    // デューティテーブルは0または1を返すので、ボリューム（0-15）を乗じる
    ctx.ch1.common.output = duty_table[ctx.ch1.duty][ctx.ch1.duty_position] * ctx.ch1.common.volume;
}

// ============================================================================
// チャンネル2のティック処理（サンプル生成）
// タイマーをデクリメントし、0に達したらデューティ位置を進める
// 出力はデューティテーブルとボリュームに基づいて計算
// Requirements: 4.6
// ============================================================================
static void tick_channel2() {
    // チャンネルが無効の場合は出力を0にして終了
    if (!ctx.ch2.common.enabled) {
        ctx.ch2.common.output = 0;
        return;
    }
    
    // タイマーをデクリメント
    if (ctx.ch2.common.timer > 0) {
        ctx.ch2.common.timer--;
    }
    
    // タイマーが0に達したら
    if (ctx.ch2.common.timer == 0) {
        // タイマーをリロード: (2048 - frequency) * 4
        ctx.ch2.common.timer = (2048 - ctx.ch2.common.frequency) * 4;
        
        // デューティ位置を進める（0-7で循環）
        ctx.ch2.duty_position = (ctx.ch2.duty_position + 1) & 0x07;
    }
    
    // 出力を計算: デューティテーブル値 * ボリューム
    // デューティテーブルは0または1を返すので、ボリューム（0-15）を乗じる
    ctx.ch2.common.output = duty_table[ctx.ch2.duty][ctx.ch2.duty_position] * ctx.ch2.common.volume;
}

// ============================================================================
// チャンネル3のティック処理（サンプル生成）
// タイマーをデクリメントし、0に達したら波形位置を進める
// 出力はWave RAMサンプルとボリュームシフトに基づいて計算
// Requirements: 5.7
// ============================================================================
static void tick_channel3() {
    // チャンネルが無効の場合は出力を0にして終了
    if (!ctx.ch3.common.enabled) {
        ctx.ch3.common.output = 0;
        return;
    }
    
    // タイマーをデクリメント
    if (ctx.ch3.common.timer > 0) {
        ctx.ch3.common.timer--;
    }
    
    // タイマーが0に達したら
    if (ctx.ch3.common.timer == 0) {
        // タイマーをリロード: (2048 - frequency) * 2
        // 注: 波形チャンネルは *2（チャンネル1,2は *4）
        ctx.ch3.common.timer = (2048 - ctx.ch3.common.frequency) * 2;
        
        // 波形位置を進める（0-31で循環）
        ctx.ch3.wave_position = (ctx.ch3.wave_position + 1) & 0x1F;
    }
    
    // Wave RAMからサンプルを読み取る
    // wave_ram[16]には32個の4ビットサンプルがパックされている
    // byte_index = wave_position / 2
    u8 byte_index = ctx.ch3.wave_position >> 1;
    u8 sample;
    
    if ((ctx.ch3.wave_position & 0x01) == 0) {
        // 偶数位置: 上位ニブル（ビット7-4）を使用
        sample = (ctx.ch3.wave_ram[byte_index] >> 4) & 0x0F;
    } else {
        // 奇数位置: 下位ニブル（ビット3-0）を使用
        sample = ctx.ch3.wave_ram[byte_index] & 0x0F;
    }
    
    // ボリュームシフトを適用
    // volume_shift: 0=100%(shift 0), 1=50%(shift 1), 2=25%(shift 2), 4=mute(shift 4)
    ctx.ch3.common.output = sample >> ctx.ch3.volume_shift;
}

// ============================================================================
// チャンネルミキシング処理
// NR51に基づく左右パンニングとNR50に基づくマスターボリュームを適用
// Requirements: 10.1, 10.2, 10.3, 10.4
// 
// NR50 (0xFF24) - マスターボリューム:
//   ビット6-4: 左ボリューム (0-7)
//   ビット2-0: 右ボリューム (0-7)
//   ビット7: Vin左有効（ほとんどのゲームで未使用）
//   ビット3: Vin右有効（ほとんどのゲームで未使用）
//
// NR51 (0xFF25) - パンニング:
//   ビット7: CH4を左に出力
//   ビット6: CH3を左に出力
//   ビット5: CH2を左に出力
//   ビット4: CH1を左に出力
//   ビット3: CH4を右に出力
//   ビット2: CH3を右に出力
//   ビット1: CH2を右に出力
//   ビット0: CH1を右に出力
// ============================================================================
static void mix_channels(int16_t *left, int16_t *right) {
    // 左右のアキュムレータを初期化
    int32_t left_acc = 0;
    int32_t right_acc = 0;
    
    // チャンネル1のミキシング
    // Requirements: 10.3
    if (ctx.nr51 & 0x10) {  // ビット4: CH1を左に
        left_acc += ctx.ch1.common.output;
    }
    if (ctx.nr51 & 0x01) {  // ビット0: CH1を右に
        right_acc += ctx.ch1.common.output;
    }
    
    // チャンネル2のミキシング
    if (ctx.nr51 & 0x20) {  // ビット5: CH2を左に
        left_acc += ctx.ch2.common.output;
    }
    if (ctx.nr51 & 0x02) {  // ビット1: CH2を右に
        right_acc += ctx.ch2.common.output;
    }
    
    // チャンネル3のミキシング
    if (ctx.nr51 & 0x40) {  // ビット6: CH3を左に
        left_acc += ctx.ch3.common.output;
    }
    if (ctx.nr51 & 0x04) {  // ビット2: CH3を右に
        right_acc += ctx.ch3.common.output;
    }
    
    // チャンネル4のミキシング
    if (ctx.nr51 & 0x80) {  // ビット7: CH4を左に
        left_acc += ctx.ch4.common.output;
    }
    if (ctx.nr51 & 0x08) {  // ビット3: CH4を右に
        right_acc += ctx.ch4.common.output;
    }
    
    // マスターボリュームを取得
    // Requirements: 10.1, 10.4
    u8 left_volume = (ctx.nr50 >> 4) & 0x07;   // ビット6-4
    u8 right_volume = ctx.nr50 & 0x07;          // ビット2-0
    
    // マスターボリュームを適用: (volume + 1) を乗算
    // ボリューム0-7 → 乗数1-8
    left_acc *= (left_volume + 1);
    right_acc *= (right_volume + 1);
    
    // 16ビットオーディオ用にスケーリング
    // 各チャンネルの最大出力は15（4ビット）
    // 4チャンネル合計の最大は 15 * 4 = 60
    // マスターボリューム最大は 8
    // 最大値: 60 * 8 = 480
    // 16ビット符号付きの範囲は -32768 to 32767
    // スケール係数: 32767 / 480 ≈ 68
    // 簡略化のため64（2^6）を使用
    left_acc *= 64;
    right_acc *= 64;
    
    // 出力を設定
    *left = (int16_t)left_acc;
    *right = (int16_t)right_acc;
}

// ============================================================================
// チャンネル4のティック処理（サンプル生成）
// タイマーをデクリメントし、0に達したらLFSRをクロック
// 出力はLFSRビット0の反転値とボリュームに基づいて計算
// Requirements: 6.6
// ============================================================================
static void tick_channel4() {
    // チャンネルが無効の場合は出力を0にして終了
    if (!ctx.ch4.common.enabled) {
        ctx.ch4.common.output = 0;
        return;
    }
    
    // タイマーをデクリメント
    if (ctx.ch4.common.timer > 0) {
        ctx.ch4.common.timer--;
    }
    
    // タイマーが0に達したら
    if (ctx.ch4.common.timer == 0) {
        // タイマーをリロード: divisor_table[divisor_code] << clock_shift
        ctx.ch4.common.timer = divisor_table[ctx.ch4.divisor_code] << ctx.ch4.clock_shift;
        
        // LFSRをクロック
        clock_lfsr();
    }
    
    // 出力を計算: LFSRビット0の反転値 * ボリューム
    // ビット0が0なら出力はhigh（1）、ビット0が1なら出力はlow（0）
    ctx.ch4.common.output = (~ctx.ch4.lfsr & 0x01) * ctx.ch4.common.volume;
}

// ============================================================================
// フレームシーケンサーのティック処理
// 8192 T-cycleごとにステップを進める（512Hz）
// ステップは0-7で循環する
// Requirements: 2.1, 2.5
// ============================================================================
static void frame_sequencer_tick() {
    // タイマーをインクリメント
    ctx.frame_sequencer_timer++;
    
    // 8192 T-cycleに達したらステップを進める
    if (ctx.frame_sequencer_timer >= 8192) {
        // タイマーをリセット
        ctx.frame_sequencer_timer = 0;
        
        // ステップを進める（0-7で循環）
        ctx.frame_sequencer_step = (ctx.frame_sequencer_step + 1) % 8;
        
        // ステップ0,2,4,6で長さカウンターをクロック
        // Requirements: 2.2
        if ((ctx.frame_sequencer_step % 2) == 0) {
            clock_length_counters();
        }
        
        // ステップ2,6でスイープユニットをクロック
        // Requirements: 2.3
        if (ctx.frame_sequencer_step == 2 || ctx.frame_sequencer_step == 6) {
            clock_sweep();
        }
        
        // ステップ7でエンベロープユニットをクロック
        // Requirements: 2.4
        if (ctx.frame_sequencer_step == 7) {
            clock_envelopes();
        }
    }
}

// ============================================================================
// T-cycle単位でAPUを進める
// Requirements: 1.2, 1.3, 10.5, 10.6, 11.3
// ============================================================================
void apu_tick() {
    // APUが無効の場合は処理しない
    if (!ctx.enabled) {
        return;
    }
    
    // フレームシーケンサーを更新
    frame_sequencer_tick();
    
    // チャンネル1のティック処理
    tick_channel1();
    
    // チャンネル2のティック処理
    tick_channel2();
    
    // チャンネル3のティック処理
    tick_channel3();
    
    // チャンネル4のティック処理
    tick_channel4();
    
    // ============================================================================
    // サンプル生成とバッファリング
    // Requirements: 10.5, 10.6, 11.3
    // ダウンサンプリング: 4.194304 MHz → 44100 Hz
    // SAMPLE_PERIOD T-cycleごとにサンプルを生成
    // ============================================================================
    
    // サンプルタイマーをインクリメント
    ctx.sample_timer++;
    
    // サンプル生成タイミングに達したら
    if (ctx.sample_timer >= SAMPLE_PERIOD) {
        // タイマーをリセット
        ctx.sample_timer = 0;
        
        // オーディオバッファが確保されていない場合はスキップ
        if (ctx.audio_buffer == NULL) {
            return;
        }
        
        // チャンネルをミキシングして左右サンプルを取得
        int16_t left_sample, right_sample;
        mix_channels(&left_sample, &right_sample);
        
        // サンプルをバッファに格納（インターリーブ形式: L, R, L, R, ...）
        ctx.audio_buffer[ctx.buffer_position] = left_sample;
        ctx.audio_buffer[ctx.buffer_position + 1] = right_sample;
        ctx.buffer_position += 2;
        
        // バッファが満杯になったらSDL2に送信
        // Requirements: 10.5, 11.3
        if (ctx.buffer_position >= ctx.buffer_size * 2) {
            // SDL_QueueAudioでバッファを送信
            if (audio_device_id != 0) {
                SDL_QueueAudio(
                    audio_device_id,
                    ctx.audio_buffer,
                    ctx.buffer_size * 2 * sizeof(int16_t)
                );
            }
            
            // バッファ位置をリセット
            ctx.buffer_position = 0;
        }
    }
}

// ============================================================================
// レジスタ読み取り
// ============================================================================
u8 apu_read(u16 address) {
    // Wave RAM (0xFF30-0xFF3F)
    if (address >= 0xFF30 && address <= 0xFF3F) {
        return ctx.ch3.wave_ram[address - 0xFF30];
    }
    
    // APUレジスタ (0xFF10-0xFF26)
    if (address >= 0xFF10 && address <= 0xFF26) {
        u8 reg_index = address - 0xFF10;
        
        // NR52 (0xFF26) - 特別処理: チャンネル有効ステータスを返す
        if (address == 0xFF26) {
            u8 value = 0;
            
            // ビット7: APU有効フラグ
            if (ctx.enabled) {
                value |= 0x80;
            }
            
            // ビット0-3: 各チャンネルの有効ステータス
            if (ctx.ch1.common.enabled) {
                value |= 0x01;
            }
            if (ctx.ch2.common.enabled) {
                value |= 0x02;
            }
            if (ctx.ch3.common.enabled) {
                value |= 0x04;
            }
            if (ctx.ch4.common.enabled) {
                value |= 0x08;
            }
            
            // ORマスクを適用（ビット4-6は未使用で1を返す）
            return value | read_masks[reg_index];
        }
        
        // その他のレジスタ: 保存された値にORマスクを適用
        return registers[reg_index] | read_masks[reg_index];
    }
    
    // 未使用アドレス (0xFF27-0xFF2F) は0xFFを返す
    return 0xFF;
}

// ============================================================================
// レジスタ書き込み
// ============================================================================
void apu_write(u16 address, u8 value) {
    // Wave RAM (0xFF30-0xFF3F)
    if (address >= 0xFF30 && address <= 0xFF3F) {
        ctx.ch3.wave_ram[address - 0xFF30] = value;
        return;
    }
    
    // APUレジスタ (0xFF10-0xFF26)
    if (address >= 0xFF10 && address <= 0xFF26) {
        u8 reg_index = address - 0xFF10;
        
        // NR52 (0xFF26) - 特別処理: APU有効/無効
        if (address == 0xFF26) {
            bool new_enabled = (value & 0x80) != 0;
            
            // APUが無効化される場合、全チャンネルレジスタをリセット
            if (ctx.enabled && !new_enabled) {
                // NR10-NR51 (0xFF10-0xFF25) をリセット
                for (int i = 0; i < 22; i++) {
                    registers[i] = 0;
                }
                
                // チャンネルを無効化
                ctx.ch1.common.enabled = false;
                ctx.ch2.common.enabled = false;
                ctx.ch3.common.enabled = false;
                ctx.ch4.common.enabled = false;
            }
            
            ctx.enabled = new_enabled;
            // NR52のビット7のみ書き込み可能（ビット0-3は読み取り専用ステータス）
            registers[reg_index] = value & 0x80;
            return;
        }
        
        // APU無効時はNR52以外への書き込みを無視
        if (!ctx.enabled) {
            return;
        }
        
        // その他のレジスタ (0xFF10-0xFF25): 値を保存
        registers[reg_index] = value;
        
        // チャンネル1レジスタ (0xFF10-0xFF14)
        // Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6
        if (address >= 0xFF10 && address <= 0xFF14) {
            write_channel1(address, value);
        }
        
        // チャンネル2レジスタ (0xFF16-0xFF19)
        // Requirements: 4.1, 4.2, 4.3, 4.4, 4.5
        if (address >= 0xFF16 && address <= 0xFF19) {
            write_channel2(address, value);
        }
        
        // チャンネル3レジスタ (0xFF1A-0xFF1E)
        // Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6
        if (address >= 0xFF1A && address <= 0xFF1E) {
            write_channel3(address, value);
        }
        
        // チャンネル4レジスタ (0xFF20-0xFF23)
        // Requirements: 6.1, 6.2, 6.3, 6.4, 6.5
        if (address >= 0xFF20 && address <= 0xFF23) {
            write_channel4(address, value);
        }
        
        // NR50 (0xFF24) - マスターボリューム
        // Requirements: 10.1
        if (address == 0xFF24) {
            ctx.nr50 = value;
        }
        
        // NR51 (0xFF25) - パンニング
        // Requirements: 10.2
        if (address == 0xFF25) {
            ctx.nr51 = value;
        }
        
        return;
    }
    
    // 未使用アドレス (0xFF27-0xFF2F) への書き込みは無視
}

// ============================================================================
// SDL2オーディオ初期化
// Requirements: 11.1, 11.2
// - 44100Hz、16ビットステレオ設定
// - オーディオバッファの確保
// - SDL_OpenAudioDeviceの呼び出し
// ============================================================================
void apu_audio_init() {
    // SDL_AudioSpecの設定
    SDL_AudioSpec desired, obtained;
    
    // 希望するオーディオ設定
    SDL_memset(&desired, 0, sizeof(desired));
    desired.freq = APU_SAMPLE_RATE;           // 44100Hz (Requirements: 11.1)
    desired.format = AUDIO_S16SYS;            // 16ビット符号付き、ネイティブエンディアン (Requirements: 11.2)
    desired.channels = 2;                      // ステレオ (Requirements: 11.2)
    desired.samples = 1024;                    // バッファサイズ（サンプル数）
    desired.callback = NULL;                   // SDL_QueueAudioを使用するためNULL
    
    // オーディオデバイスを開く
    audio_device_id = SDL_OpenAudioDevice(
        NULL,           // デフォルトデバイス
        0,              // 再生デバイス（キャプチャではない）
        &desired,
        &obtained,
        0               // 変更を許可しない
    );
    
    // エラーチェック
    // Requirements: 11.5 - 初期化失敗時はエラーログを出力し、オーディオなしで継続
    if (audio_device_id == 0) {
        printf("APU: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    
    // オーディオバッファを確保
    // バッファサイズ: APU_BUFFER_SIZE サンプル × 2チャンネル（ステレオ）
    ctx.buffer_size = APU_BUFFER_SIZE;
    ctx.audio_buffer = (int16_t *)malloc(ctx.buffer_size * 2 * sizeof(int16_t));
    
    if (ctx.audio_buffer == NULL) {
        printf("APU: Failed to allocate audio buffer\n");
        SDL_CloseAudioDevice(audio_device_id);
        audio_device_id = 0;
        return;
    }
    
    // バッファ位置を初期化
    ctx.buffer_position = 0;
    
    // オーディオ再生を開始（0 = unpause）
    SDL_PauseAudioDevice(audio_device_id, 0);
    
    printf("APU: Audio initialized (44100Hz, 16-bit stereo)\n");
}

// ============================================================================
// SDL2オーディオ終了
// Requirements: 11.4
// - SDL_CloseAudioDeviceの呼び出し
// - バッファの解放
// ============================================================================
void apu_audio_shutdown() {
    // オーディオデバイスを閉じる
    if (audio_device_id != 0) {
        SDL_CloseAudioDevice(audio_device_id);
        audio_device_id = 0;
    }
    
    // オーディオバッファを解放
    if (ctx.audio_buffer != NULL) {
        free(ctx.audio_buffer);
        ctx.audio_buffer = NULL;
    }
    
    ctx.buffer_position = 0;
    ctx.buffer_size = 0;
}
