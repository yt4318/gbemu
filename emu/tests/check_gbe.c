#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <emu.h>

#include <cpu.h>
#include <apu.h>

START_TEST(test_nothing) {
    bool b = cpu_step();
    ck_assert_uint_eq(b, false);
} END_TEST

// ============================================================================
// APU Register I/O Property Tests
// ============================================================================

/**
 * Read masks for APU registers (0xFF10-0xFF26)
 * Write-only bits return 1 when read
 */
static const u8 apu_read_masks[] = {
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

/**
 * Property 1: レジスタ書き込み・読み取りラウンドトリップ
 * 
 * For any APU register address (0xFF10-0xFF26) and valid value,
 * after writing, reading returns (written_value | read_mask).
 * 
 * **Validates: Requirements 9.1, 9.2, 9.6**
 */
START_TEST(test_apu_register_roundtrip_property) {
    // Test values to iterate over - covering various bit patterns
    static const u8 test_values[] = {
        0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
        0x0F, 0xF0, 0x55, 0xAA, 0x33, 0xCC, 0x7F, 0xFE, 0xFF
    };
    static const int num_test_values = sizeof(test_values) / sizeof(test_values[0]);
    
    // Initialize APU
    apu_init();
    
    // Enable APU by writing 0x80 to NR52 (0xFF26)
    // APU must be enabled before testing other registers
    apu_write(0xFF26, 0x80);
    
    // Verify APU is enabled
    u8 nr52_value = apu_read(0xFF26);
    ck_assert_msg((nr52_value & 0x80) != 0, 
        "APU should be enabled after writing 0x80 to NR52");
    
    // Test registers 0xFF10-0xFF25 (NR10-NR51)
    // Note: NR52 (0xFF26) is special and tested separately
    for (u16 addr = 0xFF10; addr <= 0xFF25; addr++) {
        u8 reg_index = addr - 0xFF10;
        u8 read_mask = apu_read_masks[reg_index];
        
        // Skip unused registers (0xFF15, 0xFF1F) which always return 0xFF
        if (addr == 0xFF15 || addr == 0xFF1F) {
            continue;
        }
        
        for (int i = 0; i < num_test_values; i++) {
            u8 write_value = test_values[i];
            u8 expected = write_value | read_mask;
            
            // Write value to register
            apu_write(addr, write_value);
            
            // Read back and verify
            u8 read_value = apu_read(addr);
            
            ck_assert_msg(read_value == expected,
                "Register 0x%04X: wrote 0x%02X, expected 0x%02X (value | mask 0x%02X), got 0x%02X",
                addr, write_value, expected, read_mask, read_value);
        }
    }
} END_TEST

/**
 * Property 1 (continued): NR52 special case test
 * 
 * NR52 has special behavior:
 * - Bit 7 is read/write (APU enable)
 * - Bits 0-3 are read-only (channel status)
 * - Bits 4-6 are unused and return 1 (mask 0x70)
 * 
 * **Validates: Requirements 9.1, 9.2, 9.5, 9.6**
 */
START_TEST(test_apu_nr52_special_behavior) {
    // Initialize APU
    apu_init();
    
    // Test enabling APU
    apu_write(0xFF26, 0x80);
    u8 value = apu_read(0xFF26);
    
    // Bit 7 should be set (APU enabled)
    ck_assert_msg((value & 0x80) != 0,
        "NR52 bit 7 should be set after writing 0x80");
    
    // Bits 4-6 should always be 1 (read mask 0x70)
    ck_assert_msg((value & 0x70) == 0x70,
        "NR52 bits 4-6 should always be 1, got 0x%02X", value);
    
    // Test disabling APU
    apu_write(0xFF26, 0x00);
    value = apu_read(0xFF26);
    
    // Bit 7 should be clear (APU disabled)
    ck_assert_msg((value & 0x80) == 0,
        "NR52 bit 7 should be clear after writing 0x00");
    
    // Bits 4-6 should still be 1
    ck_assert_msg((value & 0x70) == 0x70,
        "NR52 bits 4-6 should always be 1 even when APU disabled");
} END_TEST

/**
 * Property 1 (continued): APU disabled register write ignore test
 * 
 * When APU is disabled (NR52 bit 7 = 0), writes to other registers
 * should be ignored.
 * 
 * **Validates: Requirements 1.4, 9.2**
 */
START_TEST(test_apu_disabled_ignores_writes) {
    // Initialize APU
    apu_init();
    
    // Ensure APU is disabled
    apu_write(0xFF26, 0x00);
    
    // Try to write to various registers
    apu_write(0xFF12, 0xFF);  // NR12
    apu_write(0xFF17, 0xFF);  // NR22
    apu_write(0xFF24, 0xFF);  // NR50
    apu_write(0xFF25, 0xFF);  // NR51
    
    // Enable APU to read the values
    apu_write(0xFF26, 0x80);
    
    // Values should be 0 (or masked) since writes were ignored
    // NR12 has mask 0x00, so should read 0x00
    u8 nr12 = apu_read(0xFF12);
    ck_assert_msg(nr12 == 0x00,
        "NR12 should be 0x00 after ignored write, got 0x%02X", nr12);
    
    // NR22 has mask 0x00, so should read 0x00
    u8 nr22 = apu_read(0xFF17);
    ck_assert_msg(nr22 == 0x00,
        "NR22 should be 0x00 after ignored write, got 0x%02X", nr22);
    
    // NR50 has mask 0x00, so should read 0x00
    u8 nr50 = apu_read(0xFF24);
    ck_assert_msg(nr50 == 0x00,
        "NR50 should be 0x00 after ignored write, got 0x%02X", nr50);
    
    // NR51 has mask 0x00, so should read 0x00
    u8 nr51 = apu_read(0xFF25);
    ck_assert_msg(nr51 == 0x00,
        "NR51 should be 0x00 after ignored write, got 0x%02X", nr51);
} END_TEST

Suite *stack_suite() {
    Suite *s = suite_create("emu");
    TCase *tc = tcase_create("core");

    tcase_add_test(tc, test_nothing);
    suite_add_tcase(s, tc);

    // APU Register I/O Property Tests
    // Feature: gameboy-apu, Property 1: レジスタ書き込み・読み取りラウンドトリップ
    TCase *tc_apu = tcase_create("apu_register_io");
    tcase_add_test(tc_apu, test_apu_register_roundtrip_property);
    tcase_add_test(tc_apu, test_apu_nr52_special_behavior);
    tcase_add_test(tc_apu, test_apu_disabled_ignores_writes);
    suite_add_tcase(s, tc_apu);

    return s;
}

int main() {
    Suite *s = stack_suite();
    SRunner *sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int nf = srunner_ntests_failed(sr);

    srunner_free(sr);

    return nf == 0 ? 0 : -1;
}

