#include <cpu.h>
#include <bus.h>
#include <emu.h>

extern cpu_context ctx;

void fetch_data() {
    ctx.mem_dest = 0;
    ctx.dest_is_mem = false;
    
    if (ctx.cur_inst == NULL) {
        return;
    }

    switch(ctx.cur_inst->mode) {
        case AM_IMP: return; //read no data

        case AM_R: //read reg_1
            ctx.fetched_data = cpu_read_reg(ctx.cur_inst->reg_1);
            return;
        
        case AM_R_R: //read reg_2
            ctx.fetched_data = cpu_read_reg(ctx.cur_inst -> reg_2);
            return;
        
        case AM_R_D8: //read 8bit data from pc address
            ctx.fetched_data = bus_read(ctx.regs.pc);
            emu_cycles(1);
            ctx.regs.pc++;
            return;
        
        case AM_R_D16: //same as AM_D16
        case AM_D16: {//read 16bit data from pc address
            u16 lo = bus_read (ctx.regs.pc);
            emu_cycles(1);
            u16 hi = bus_read(ctx.regs.pc + 1);
            emu_cycles(1);
            ctx.fetched_data = lo | (hi << 8);
            ctx.regs.pc += 2;
            return;
        }

        case AM_MR_R: // read data from reg_2 and fetch destination memory address from reg_1
            ctx.fetched_data = cpu_read_reg(ctx.cur_inst->reg_2);
            ctx.mem_dest = cpu_read_reg(ctx.cur_inst->reg_1);
            ctx.dest_is_mem = true;

            if(ctx.cur_inst->reg_1 == RT_C) {
                ctx.mem_dest |= 0xFF00;
            }

            return;

        case AM_R_MR: {//fetch memory address from reg_2 and read data from address
            u16 addr = cpu_read_reg(ctx.cur_inst->reg_2);

            if(ctx.cur_inst->reg_2 == RT_C) {
                addr |= 0xFF00;
            }

            ctx.fetched_data = bus_read(addr);
            emu_cycles(1);
            return;
        }

        case AM_R_HLI: //read data from reg_2 address and increment HL register
            ctx.fetched_data = bus_read(cpu_read_reg(ctx.cur_inst->reg_2));
            emu_cycles(1);
            cpu_set_reg(RT_HL, cpu_read_reg(RT_HL + 1));
            return;

        case AM_R_HLD: //read data from reg_2 address and decrement HL register
            ctx.fetched_data = bus_read(cpu_read_reg(ctx.cur_inst->reg_2));
            emu_cycles(1);
            cpu_set_reg(RT_HL, cpu_read_reg(RT_HL - 1));
            return;

        case AM_HLI_R: //read data from reg_2 and fetch destination memory address from reg_1 and increment HL register
            ctx.fetched_data = cpu_read_reg(ctx.cur_inst->reg_2);
            ctx.mem_dest = cpu_read_reg(ctx.cur_inst->reg_1);
            ctx.dest_is_mem = true;
            cpu_set_reg(RT_HL, cpu_read_reg(RT_HL) + 1);
            return;

        case AM_HLD_R: //read data from reg_2 and fetch destination memory address from reg_1 and decrement HL register
            ctx.fetched_data = cpu_read_reg(ctx.cur_inst->reg_2);
            ctx.mem_dest = cpu_read_reg(ctx.cur_inst->reg_1);
            ctx.dest_is_mem = true;
            cpu_set_reg(RT_HL, cpu_read_reg(RT_HL) - 1);
            return;
        
        case AM_R_A8: //read data from pc address
            ctx.fetched_data = bus_read(ctx.regs.pc);
            emu_cycles(1);
            ctx.regs.pc++;
            return;
        
        case AM_A8_R: //fetch destination memory address from pc
            ctx.mem_dest = bus_read(ctx.regs.pc) | 0xFF00;
            ctx.dest_is_mem = true;
            emu_cycles(1);
            ctx.regs.pc++;
            return;
        
        case AM_HL_SPR: //same as AM_R_A8
            ctx.fetched_data = bus_read(ctx.regs.pc);
            emu_cycles(1);
            ctx.regs.pc++;
            return;
        
        case AM_D8: //same as AM_R_A8
            ctx.fetched_data = bus_read(ctx.regs.pc);
            emu_cycles(1);
            ctx.regs.pc++;
            return;

        case AM_A16_R: //same as AM_D16_R
        case AM_D16_R: {//read data from reg_2 and fetch 16 bit destination memory address from pc
            u16 lo = bus_read(ctx.regs.pc);
            emu_cycles(1);
            u16 hi = bus_read(ctx.regs.pc + 1);
            emu_cycles(1);
            ctx.mem_dest = lo | (hi << 8);
            ctx.dest_is_mem = true;
            ctx.regs.pc += 2;
            ctx.fetched_data = cpu_read_reg(ctx.cur_inst->reg_2);
            return;      
        }

        case AM_MR_D8: //read data from pc address and fetch destination memory address from reg_1
            ctx.fetched_data = bus_read(ctx.regs.pc);
            emu_cycles(1);
            ctx.regs.pc++;
            ctx.mem_dest = cpu_read_reg(ctx.cur_inst->reg_1);
            ctx.dest_is_mem = true;
            return;
        
        case AM_MR: //fetch destination memory address from reg_1 and read data from reg_1 address
            ctx.mem_dest = cpu_read_reg(ctx.cur_inst->reg_1);
            ctx.dest_is_mem = true;
            ctx.fetched_data = bus_read(cpu_read_reg(ctx.cur_inst->reg_1));
            emu_cycles(1);
            return;
        
        case AM_R_A16: {//read data from 16 bit pc address
            u16 lo = bus_read(ctx.regs.pc);
            emu_cycles(1);
            u16 hi = bus_read(ctx.regs.pc + 1);
            emu_cycles(1);
            u16 addr = lo | (hi << 8);
            ctx.regs.pc += 2;
            ctx.fetched_data = bus_read(addr);
            emu_cycles(1);
            return;
        }
        
        default:
            printf("Unknown Addressing Mode! %d (%02X)\n", ctx.cur_inst->mode, ctx.cur_opcode);
            exit(-7);
            return;
    }
}