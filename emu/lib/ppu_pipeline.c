#include <ppu.h>
#include <lcd.h>
#include <bus.h>

bool window_visible() {
    return LCDC_WIN_ENABLE && lcd_get_context()->win_x >= 0 &&
    lcd_get_context()->win_x <= 166 && lcd_get_context()->win_y >= 0 &&
    lcd_get_context()->win_y < YRES;
}

void pixel_fifo_push(u32 value) {
    fifo_entry *next = malloc(sizeof(fifo_entry));
    next->next = NULL;
    next->value = value;

    if(!ppu_get_context()->pfc.pixel_fifo.head) {
        ppu_get_context()->pfc.pixel_fifo.head = ppu_get_context()->pfc.pixel_fifo.tail = next;
    } else {
        ppu_get_context()->pfc.pixel_fifo.tail->next = next;
        ppu_get_context()->pfc.pixel_fifo.tail = next;
    }
    ppu_get_context()->pfc.pixel_fifo.size++;
}

u32 pixel_fifo_pop() {
    if(ppu_get_context()->pfc.pixel_fifo.size <= 0) {
        fprintf(stderr, "ERR IN PIXEL FIFO!\n");
        exit(-8);
    }

    fifo_entry *popped = ppu_get_context()->pfc.pixel_fifo.head;
    ppu_get_context()->pfc.pixel_fifo.head = popped->next;
    ppu_get_context()->pfc.pixel_fifo.size--;
    u32 val = popped->value;
    free(popped);

    return val;
}

u32 fetch_sprite_pixels(int bit, u32 color, u8 bg_color) {
    for (int i=0; i<ppu_get_context()->fetched_entry_count; i++) {
        int sp_x = (ppu_get_context()->fetched_entries[i].x - 8) + 
            ((lcd_get_context()->scroll_x % 8));
        
        if (sp_x + 8 < ppu_get_context()->pfc.fifo_x) {
            //past pixel point already...
            continue;
        }

        //既にFIFOにプッシュされたスプライトピクセルの数
        int offset = ppu_get_context()->pfc.fifo_x - sp_x;

        if (offset < 0 || offset > 7) {
            //out of bounds..
            continue;
        }

        //既にFIFOにプッシュされたスプライトピクセルの数
        bit = (7 - offset);

        if (ppu_get_context()->fetched_entries[i].f_x_flip) {
            bit = offset;
        }

        u8 hi = !!(ppu_get_context()->pfc.fetch_entry_data[i * 2] & (1 << bit));
        u8 lo = !!(ppu_get_context()->pfc.fetch_entry_data[(i * 2) + 1] & (1 << bit)) << 1;

        bool bg_priority = ppu_get_context()->fetched_entries[i].f_bgp;

        if (!(hi|lo)) {
            //transparent
            continue;
        }

        if (!bg_priority || bg_color == 0) {
            color = (ppu_get_context()->fetched_entries[i].f_pn) ? 
                lcd_get_context()->sp2_colors[hi|lo] : lcd_get_context()->sp1_colors[hi|lo];

            //スプライトの重なりを考慮した処理
            //最前面のスプライトから順に処理してもし不透明なピクセルを描画したらそこでforループを抜けて背面のスプライトは描画しない
            if (hi|lo) {
                break;
            }
        }
    }

    return color;
}

bool pipeline_fifo_add() {
    if (ppu_get_context()->pfc.pixel_fifo.size > 8) {
        return false;
    }

    //スキャンライン上で少なくとも1回フェッチが行われていることを確認
    int x = ppu_get_context()->pfc.fetch_x - (8 - (lcd_get_context()->scroll_x % 8));

    //フェッチしたデータからPixelカラーを計算してPixel FIFOにプッシュ
    for(int i=0; i<8; i++) {
        int bit = 7-i;
        u8 lo = !!(ppu_get_context()->pfc.bgw_fetch_data[1] & (1 << bit));
        u8 hi = !!(ppu_get_context()->pfc.bgw_fetch_data[2] & (1 << bit)) << 1;
        u32 color = lcd_get_context()->bg_colors[hi | lo];

        if(!LCDC_BGW_ENABLE) {
            color = lcd_get_context()->bg_colors[0];
        }

        if(LCDC_OBJ_ENABLE) {
            color = fetch_sprite_pixels(bit, color, hi | lo);
        }

        if (x >= 0) {
            pixel_fifo_push(color);
            ppu_get_context()->pfc.fifo_x++;
        }
    }
    return true;
}

void pipeline_load_sprite_tile() {
    oam_line_entry *le = ppu_get_context()->line_sprites;
    //ラインエントリがフェッチ中の8ピクセルに含まれるか判定
    while(le) {
        int sp_x = (le->entry.x - 8);
        if((sp_x >= ppu_get_context()->pfc.fetch_x && sp_x < ppu_get_context()->pfc.fetch_x + 8) ||
            ((sp_x + 8) >= ppu_get_context()->pfc.fetch_x && (sp_x + 8) < ppu_get_context()->pfc.fetch_x + 8)) {
                ppu_get_context()->fetched_entries[ppu_get_context()->fetched_entry_count++] = le->entry;
            }

            le = le->next;

        if((!le || ppu_get_context()->fetched_entry_count >= 3)) {
            ////最大3ラインエントリ
            break;
        }
    }
}

void pipeline_load_sprite_data(u8 offset) {
    int cur_y = lcd_get_context()->ly;
    u8 sprite_height = LCDC_OBJ_HEIGHT;
    //ラインエントリ内のYバイトオフセットを計算
    //((スキャンラインのY座標+スプライトのハードウェアオフセット) - スプライトのY座標)　× 2BPPで計算
    for(int i=0; i<ppu_get_context()->fetched_entry_count; i++) {
        u8 ty = ((cur_y + 16) - ppu_get_context()->fetched_entries[i].y) * 2;

        if(ppu_get_context()->fetched_entries[i].f_y_flip) {
            ty = ((sprite_height * 2) - 2) - ty;
        }

        u8 tile_index = ppu_get_context()->fetched_entries[i].tile;

        //8×16の場合は偶数インデックスから始まるようにするため0bit目を無視する
        if(sprite_height == 16) {
            tile_index &= ~(1);
        }

        //各ラインエントリごとにスキャンライン上のタイルデータ(2BPP分)を取得
        ppu_get_context()->pfc.fetch_entry_data[(i * 2) + offset] = bus_read(0x8000 + (tile_index * 16) + ty + offset);
    }
}

void pipeline_load_window_tile() {
    if (!window_visible()) {
        return;
    }
    
    u8 window_y = lcd_get_context()->win_y;

    if (ppu_get_context()->pfc.fetch_x + 7 >= lcd_get_context()->win_x &&
            ppu_get_context()->pfc.fetch_x + 7 < lcd_get_context()->win_x + XRES + 14) {
        if (lcd_get_context()->ly >= window_y && lcd_get_context()->ly < window_y + YRES) {
            u8 w_tile_y = ppu_get_context()->window_line / 8;

            ppu_get_context()->pfc.bgw_fetch_data[0] = bus_read(LCDC_WIN_MAP_AREA + 
                ((ppu_get_context()->pfc.fetch_x + 7 - lcd_get_context()->win_x) / 8) +
                (w_tile_y * 32));

            if (LCDC_BGW_DATA_AREA == 0x8800) {
                ppu_get_context()->pfc.bgw_fetch_data[0] += 128;
            }
        }
    }
}

void pipeline_fetch() {
    switch(ppu_get_context()->pfc.cur_fetch_state) {
        case FS_TILE: {
            ppu_get_context()->fetched_entry_count = 0;

            //タイルマップからピクセルフェッチャーがのっているタイルIDを取得
            //タイルマップは32×32タイル。1タイルは8×8ピクセル
            if(LCDC_BGW_ENABLE) {
                ppu_get_context()->pfc.bgw_fetch_data[0] = bus_read(LCDC_BG_MAP_AREA + (ppu_get_context()->pfc.map_x / 8) + ((ppu_get_context()->pfc.map_y / 8) * 32));

                //タイルデータ領域が0x8800の場合はタイルIDに128を足す
                if(LCDC_BGW_DATA_AREA == 0x8800) {
                    ppu_get_context()->pfc.bgw_fetch_data[0] += 128;
                }

                pipeline_load_window_tile();
            }

            if(LCDC_OBJ_ENABLE && ppu_get_context()->line_sprites) {
                pipeline_load_sprite_tile();
            }

            ppu_get_context()->pfc.cur_fetch_state = FS_DATA0;
            ppu_get_context()->pfc.fetch_x += 8;
        } break;

        //タイルデータ領域からスキャンラインに対応する2BPPをフェッチする
        //1Tile(16バイト)×タイルID + タイル内Yオフセット
        case FS_DATA0: {
            ppu_get_context()->pfc.bgw_fetch_data[1] = bus_read(LCDC_BGW_DATA_AREA + (ppu_get_context()->pfc.bgw_fetch_data[0] * 16) + ppu_get_context()->pfc.tile_y);

            pipeline_load_sprite_data(0);

            ppu_get_context()->pfc.cur_fetch_state = FS_DATA1;
        } break;

        case FS_DATA1: {
            ppu_get_context()->pfc.bgw_fetch_data[2] = bus_read(LCDC_BGW_DATA_AREA + (ppu_get_context()->pfc.bgw_fetch_data[0] * 16) + ppu_get_context()->pfc.tile_y + 1);

            pipeline_load_sprite_data(1);
            
            ppu_get_context()->pfc.cur_fetch_state = FS_IDLE;
        } break;

        case FS_IDLE: {
            ppu_get_context()->pfc.cur_fetch_state = FS_PUSH;
        } break;

        case FS_PUSH: {
            if(pipeline_fifo_add()) {
                ppu_get_context()->pfc.cur_fetch_state = FS_TILE;
            }
        } break;
    }
}

void pipeline_push_pixel() {
    //Pixel FIFOの大きさが8より大きくなったらPOPする
    if(ppu_get_context()->pfc.pixel_fifo.size > 8) {
        u32 pixel_data = pixel_fifo_pop();

        //Pixel FIFOにはタイル単位でピクセルがプッシュされるので、スクロール範囲内にない場合はピクセルを破棄する
        //LCDスクリーン範囲内にある場合はピクセルをラインの端から順に敷き詰める
        if(ppu_get_context()->pfc.line_x >= (lcd_get_context()->scroll_x % 8)){
            ppu_get_context()->video_buffer[ppu_get_context()->pfc.pushed_x + (lcd_get_context()->ly * XRES)] = pixel_data;
            ppu_get_context()->pfc.pushed_x++;
        }
        ppu_get_context()->pfc.line_x++;
    }
}

void pipeline_process() {
    //SCX/SCYの座標にLCDスクリーン内のオフセットを足して256×256のマップ上のフェッチャー座標を計算
    ppu_get_context()->pfc.map_y = (lcd_get_context()->ly + lcd_get_context()->scroll_y);
    ppu_get_context()->pfc.map_x = (ppu_get_context()->pfc.fetch_x + lcd_get_context()->scroll_x);
    ////SCYにLYを足して8で割った余りに2BPPをかけることでことでタイル内のY座標バイトオフセットを計算
    ppu_get_context()->pfc.tile_y = ((lcd_get_context()->ly + lcd_get_context()->scroll_y) % 8) * 2;
    
    //line_ticksが偶数のときにfetch
    if(!(ppu_get_context()->line_ticks & 1)) {
        pipeline_fetch();
    }

    //ビデオバッファーにピクセルをプッシュ
    pipeline_push_pixel();
}

void pipeline_fifo_reset() {
    while(ppu_get_context()->pfc.pixel_fifo.size) {
        pixel_fifo_pop();
    }

    ppu_get_context()->pfc.pixel_fifo.head = 0;
}