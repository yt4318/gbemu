#include <ppu.h>
#include <lcd.h>
#include <cpu.h>
#include <interrupts.h>
#include <ppu_sm.h>
#include <common.h>
#include <string.h>

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

void load_line_sprites() {
    int cur_y = lcd_get_context()->ly;

    u8 sprite_height = LCDC_OBJ_HEIGHT;
    memset(ppu_get_context()->line_entry_array, 0, sizeof(ppu_get_context()->line_entry_array));

    //最大40個のスプライトそれぞれがスキャンライン上にあるか個別に確認
    for(int i=0; i<40; i++) {
        oam_entry e = ppu_get_context()->oam_ram[i];

        if(!e.x) {
            continue;
        }

        //スキャンライン上のスプライトは最大10個
        if(ppu_get_context()->line_sprite_count>=10) {
            break;
        }

        //スプライトがスキャンラインをまたいでいる場合にラインエントリを追加する
        if(e.y - 16 <= cur_y && e.y - 16 + sprite_height > cur_y) {
            //ラインエントリは最大10で固定なのでmalloc()/free()しないで予め配列でメモリを確保してポインタで参照する
            oam_line_entry *entry = &ppu_get_context()->line_entry_array[ppu_get_context()->line_sprite_count++];
            
            entry->entry = e;
            entry->next = NULL;

            //line_spritesは先頭のラインエントリを指す
            //以下先頭にラインエントリを追加する場合の処理
            if(!ppu_get_context()->line_sprites || ppu_get_context()->line_sprites->entry.x > e.x) {
                entry->next = ppu_get_context()->line_sprites;
                ppu_get_context()->line_sprites = entry;
                continue;
            }

            //以下先頭以外の位置にラインエントリを挿入する処理
            //ラインエントリはx座標が小さい順になるように並べる
            oam_line_entry *le = ppu_get_context()->line_sprites;
            oam_line_entry *prev = le;

            while(le) {

                if(le->entry.x > e.x) {
                    prev->next = entry;
                    entry->next = le;
                    break;
                }

                if(!le->next) {
                    le->next = entry;
                    break;
                }

                prev = le;
                le = le->next;
            }
        }
    }
}

//line_ticksが80以上になったらMODE_XFERに遷移。
//Pixel FIFOを初期化
void ppu_mode_oam() {
    if(ppu_get_context()->line_ticks >= 80) {
        LCDS_MODE_SET(MODE_XFER);

        ppu_get_context()->pfc.cur_fetch_state = FS_TILE;
        ppu_get_context()->pfc.line_x = 0;
        ppu_get_context()->pfc.fetch_x = 0;
        ppu_get_context()->pfc.pushed_x = 0;
        ppu_get_context()->pfc.fifo_x = 0;
    }

    if(ppu_get_context()->line_ticks == 1) {
        ppu_get_context()->line_sprites = 0;
        ppu_get_context()->line_sprite_count = 0;

        load_line_sprites();
    }
}

//パイプライン処理を実行
//現在のラインで転送されたピクセルの数が画面の横幅(XRES)に達したら、FIFOをリセットして、MODE_HBLANKに遷移
//HBLANK割り込みが有効な場合STAT割り込みを実行
void ppu_mode_xfer() {
    pipeline_process();

    if(ppu_get_context()->pfc.pushed_x >= XRES) {
        pipeline_fifo_reset();

        LCDS_MODE_SET(MODE_HBLANK);

        if(LCDS_STAT_INT(SS_HBLANK)) {
            cpu_request_interrupt(IT_LCD_STAT);
        }
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