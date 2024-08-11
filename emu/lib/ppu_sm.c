#include <ppu.h>
#include <lcd.h>
#include <cpu.h>
#include <interrupts.h>
#include <ppu_sm.h>
#include <common.h>

//lyをインクリメント。
//lyがly_compareに等しい場合はSTAT割り込みをリクエスト。
void increment_ly() {
    lcd_get_context()->ly++;

    if (lcd_get_context()->ly == lcd_get_context()->ly_compare) {
        LCDS_LYC_SET(1);

        if (LCDS_STAT_INT(SS_LYC)) {
            cpu_request_interrupt(IT_LCD_STAT);
        }
    } else {
        LCDS_LYC_SET(0);
    }
}

//line_ticksが80以上になったらMODE_XFERに遷移。
void ppu_mode_oam() {
    if(ppu_get_context()->line_ticks >= 80) {
        LCDS_MODE_SET(MODE_XFER);
    }
}

//line_ticksが80 + 172以上になったらMODE_HBLANKに遷移。
void ppu_mode_xfer() {
    if(ppu_get_context()->line_ticks >= 80 + 172) {
        LCDS_MODE_SET(MODE_HBLANK);
    }
}

//line_ticksがTICKS_PER_LINEをこえたらlyをインクリメント。
//lyがLINES_PER_FRAME以上になったらMODE_OAMに遷移して、lyを0にリセット。
void ppu_mode_vblank() {
    if(ppu_get_context()->line_ticks >= TICKS_PER_LINE) {
        increment_ly();

        if(lcd_get_context()->ly >= LINES_PER_FRAME) {
            LCDS_MODE_SET(MODE_OAM);
            lcd_get_context()->ly = 0;
        }

        ppu_get_context()->line_ticks = 0;
    }
}

static u32 target_frame_time = 1000 / 60; //1フレームの秒数ターゲット。Target frame time = 1000ms(1 second) / 60 FPS (Target FPS)
static long prev_frame_time = 0;
static long start_timer = 0;
static long frame_count = 0;

//line_ticksがTICKS_PER_LINEをこえたらlyをインクリメント。
//lyがYRESより小さい場合はMODE_OAMに遷移。
//lyがYRES以上になったらMODE_VBLANKに遷移して以下を実行。
//1. VBLANK割り込みをリクエスト。
//2. LCDSレジスタでVBLANK割り込みが有効な場合はSTAT割り込みをリクエスト。
//3. FPS関連処理を実行
// 3-1. フレーム時間が60FPS想定のターゲットフレーム時間より小さい場合はdelayさせる
// 3-2. 1000ms経過したらフレームカウントを表示して初期化。

void ppu_mode_hblank() {
    if (ppu_get_context()->line_ticks >= TICKS_PER_LINE) {
        increment_ly();

        if(lcd_get_context()->ly >= YRES) {
            LCDS_MODE_SET(MODE_VBLANK);

            cpu_request_interrupt(IT_VBLANK);

            if(LCDS_STAT_INT(SS_VBLANK)) {
                cpu_request_interrupt(IT_LCD_STAT);
            }

            ppu_get_context()->current_frame++;

            u32 end = get_ticks();
            u32 frame_time = end - prev_frame_time;

            if(frame_time < target_frame_time) {
                delay((target_frame_time - frame_time));
            }

            if(end - start_timer >= 1000) {
                u32 fps = frame_count;
                start_timer = end;
                frame_count = 0;

                printf("FPS: %d\n", fps);
            }

            frame_count++;
            prev_frame_time = get_ticks();
        }  else {
            LCDS_MODE_SET(MODE_OAM);
        }
        ppu_get_context()->line_ticks = 0;
    }
}