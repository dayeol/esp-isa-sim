#include "gemmini.h"
#include "mmu.h"
#include "trap.h"
#include <stdexcept>
#include <iostream>
#include <assert.h>

REGISTER_EXTENSION(gemmini, []() { return new gemmini_t; })

void gemmini_state_t::reset()
{
  enable = true;
  mode = OS;
  act = NONE;
  acc_shift = 0;
  sys_shift = 0;
  relu6_shift = 0;
  output_sp_addr = 0;
  // EE290DAYEOL
	locked = 0;
	locked_satp = 0;
	load_stride = dim * sizeof(input_t);
  store_stride = dim * sizeof(input_t);
  spad = new std::vector<std::vector<input_t>>(sp_matrices*dim, std::vector<input_t>(dim));
  for (size_t row = 0; row < sp_matrices*dim; ++row) {
    for (size_t elem = 0; elem < dim; ++elem) {
      spad->at(row).at(elem) = 0;
    }
  }
  pe_state = new std::vector<std::vector<accum_t>>(dim, std::vector<accum_t>(dim));
  accumulator = new std::vector<std::vector<accum_t>>(accum_rows, std::vector<accum_t>(dim));
  for (size_t row = 0; row < accum_rows; ++row) {
    for (size_t elem = 0; elem < dim; ++elem) {
      accumulator->at(row).at(elem) = 0;
    }
  }

  printf("Gemmini extension configured with:\n");
  printf("    dim = %u\n", dim);
}

void gemmini_t::reset() {
  gemmini_state.reset();
}

template <class T>
T gemmini_t::read_from_dram(reg_t addr) {
  T value = 0;
  for (size_t byte_idx = 0; byte_idx < sizeof(T); ++byte_idx) {
    value |= p->get_mmu()->load_uint8(addr + byte_idx) << (byte_idx*8);
  }
  return value;
}

template <class T>
void gemmini_t::write_to_dram(reg_t addr, T data) {
  for (size_t byte_idx = 0; byte_idx < sizeof(T); ++byte_idx) {
    p->get_mmu()->store_uint8(addr + byte_idx, (data >> (byte_idx*8)) & 0xFF);
  }
}

// Move a gemmini block from DRAM at dram_addr (byte addr) to
// the scratchpad/accumulator at sp_addr (gemmini-row addressed)
void gemmini_t::mvin(reg_t dram_addr, reg_t sp_addr) {
  bool const accumulator = (((sp_addr >> 31) & 0x1) == 1);
  auto const base_row_addr = (sp_addr & 0x3FFFFFFF); // Strip accumulator addressing bits [31:30]
  auto const blocks = (sp_addr >> addr_len);
  assert(blocks >= 1);

  dprintf("GEMMINI: mvin - %02lx blocks from 0x%08lx to addr 0x%08lx\n", blocks, dram_addr, sp_addr);

  for (size_t block = 0; block < blocks; ++block) {
    for (size_t i = 0; i < dim; ++i) {
      auto const dram_row_addr = dram_addr + i*gemmini_state.load_stride;
      for (size_t j = 0; j < dim; ++j) {
        if (accumulator) {
          auto const dram_byte_addr = dram_row_addr + j*sizeof(accum_t) + block*dim*sizeof(accum_t);
          auto value = read_from_dram<accum_t>(dram_byte_addr);
          gemmini_state.accumulator->at(base_row_addr + i + block*dim).at(j) = value;
          dprintf("%d ",gemmini_state.accumulator->at(base_row_addr + i + block*dim).at(j));
        } else {
          auto const dram_byte_addr = dram_row_addr + j*sizeof(input_t) + block*dim*sizeof(input_t);
          auto value = read_from_dram<input_t>(dram_byte_addr);
          gemmini_state.spad->at(base_row_addr + i + block*dim).at(j) = value;
          dprintf("%d ",gemmini_state.spad->at(base_row_addr + i + block*dim).at(j));
        }
      }
      dprintf("\n");
    }
    dprintf("\n");
  }
}

void gemmini_t::mvout(reg_t dram_addr, reg_t sp_addr) {
  bool const accumulator = (((sp_addr >> 31) & 0x1) == 1);
  auto const base_row_addr = (sp_addr & 0x3FFFFFFF); // Strip accumulator addressing bits [31:30]

  dprintf("GEMMINI: mvout - block from 0x%08lx to addr 0x%08lx\n", base_row_addr, dram_addr);

  for (size_t i = 0; i < dim; ++i) {
    auto const dram_row_addr = dram_addr + i*gemmini_state.store_stride;
    for (size_t j = 0; j < dim; ++j) {
      if (accumulator) { // Apply shift and activation when moving out of accumulator
        accum_t acc_value = gemmini_state.accumulator->at(base_row_addr + i).at(j);
        auto shifted = rounding_saturating_shift<input_t>(acc_value, gemmini_state.acc_shift);
        input_t activated = apply_activation(shifted); // Activation is always applied in either WS/OS mode

        auto const dram_byte_addr = dram_row_addr + j*sizeof(input_t);
        write_to_dram<input_t>(dram_byte_addr, activated);
        dprintf("%d ", activated);
      } else { // Scratchpad, write to DRAM directly
        auto const dram_byte_addr = dram_row_addr + j*sizeof(input_t);
        input_t value = gemmini_state.spad->at(base_row_addr + i).at(j);
        write_to_dram<input_t>(dram_byte_addr, value);
        dprintf("%d ", value);
      }
    }
    dprintf("\n");
  }
}

void gemmini_t::preload(reg_t bd_addr, reg_t c_addr) {
  // TODO: rename these state variables
  gemmini_state.preload_sp_addr = static_cast<uint32_t>(bd_addr & 0xFFFFFFFF);
  gemmini_state.output_sp_addr = static_cast<uint32_t>(c_addr & 0xFFFFFFFF);
  dprintf("GEMMINI: preload - scratchpad output addr = 0x%08x, scratchpad preload addr = 0x%08x\n",
            gemmini_state.output_sp_addr, gemmini_state.preload_sp_addr);
}

void gemmini_t::setmode(reg_t rs1, reg_t rs2) {
  if ((rs1 & 0b11) == 0) { // rs1[1:0] == 2'b00, config_ex, configure execute pipeline
    gemmini_state_t::Dataflow new_mode;
    gemmini_state_t::Activation new_act;
    reg_t new_acc_shift, new_sys_shift, new_relu6_shift;

    auto rs1_2 = (rs1 >> 2) & 0b1; // extract rs1[2], 0 = output stationary, 1 = weight stationary
    if (rs1_2 == 0) {
      new_mode = gemmini_state_t::OS;
    } else {
      new_mode = gemmini_state_t::WS;
    }

    auto rs1_4_3 = (rs1 >> 3) & 0b11; // extract rs1[4:3], 0 = no activation, 1 = ReLU, 2 = ReLU6
    if (rs1_4_3 == 0) {
      new_act = gemmini_state_t::NONE;
    } else if (rs1_4_3 == 1) {
      new_act = gemmini_state_t::RELU;
    } else if (rs1_4_3 == 2) {
      new_act = gemmini_state_t::RELU6;
    } else {
      assert(false);
    }

    new_acc_shift = (rs1 >> 32) & 0xFFFFFFFF;
    new_sys_shift = (rs2) & 0xFFFFFFFF;
    new_relu6_shift = (rs2 >> 32) & 0xFFFFFFFF;

    dprintf("GEMMINI: config_ex - set dataflow mode from %d to %d\n", gemmini_state.mode, new_mode);
    dprintf("GEMMINI: config_ex - set activation function from %d to %d\n", gemmini_state.act, new_act);
    dprintf("GEMMINI: config_ex - set acc_shift from %lu to %lu\n", gemmini_state.acc_shift, new_acc_shift);
    dprintf("GEMMINI: config_ex - set sys_shift from %lu to %lu\n", gemmini_state.sys_shift, new_sys_shift);
    dprintf("GEMMINI: config_ex - set relu6_shift from %lu to %lu\n", gemmini_state.relu6_shift, new_relu6_shift);

    gemmini_state.mode = new_mode;
    gemmini_state.act = new_act;

    assert(new_acc_shift >= 0 && new_acc_shift < sizeof(accum_t)*8);
    assert(new_sys_shift >= 0 && new_sys_shift < sizeof(output_t)*8);
    assert(new_relu6_shift >= 0);
    gemmini_state.acc_shift = new_acc_shift;
    gemmini_state.sys_shift = new_sys_shift;
    gemmini_state.relu6_shift = new_relu6_shift;
  } else if ((rs1 & 0b11) == 1) { // rs1[1:0] == 2'b01, config_mvin, configure load pipeline
    dprintf("GEMMINI: config_mvin - set load stride from %lu to %lu\n", gemmini_state.load_stride, rs2);
    gemmini_state.load_stride = rs2;
  } else if ((rs1 & 0b11) == 2) { // rs1[1:0] == 2'b10, config_mvout, configure store pipeline
    dprintf("GEMMINI: config_mvout - set store stride from %lu to %lu\n", gemmini_state.store_stride, rs2);
    gemmini_state.store_stride = rs2;
  }
}

void gemmini_t::compute(reg_t a_addr, reg_t bd_addr, bool preload) {
  auto a_addr_real = static_cast<uint32_t>(a_addr & 0xFFFFFFFF);
  auto bd_addr_real = static_cast<uint32_t>(bd_addr & 0xFFFFFFFF);

  dprintf("GEMMINI: compute - preload = %d, scratchpad A addr = 0x%08x,"
           "scratchpad B addr 0x%08x\n", preload, a_addr_real, bd_addr_real);

  // Preload
  if (preload) {
    dprintf("GEMMINI: compute - PEs after preloading:\n");
    for (size_t i = 0; i < dim; i++) {
      for (size_t j = 0; j < dim; j++) {
        // TODO: Handle preloads from accumulator, values are shifted and activated before preload
        if (~gemmini_state.preload_sp_addr != 0) {
          assert(((gemmini_state.preload_sp_addr >> 30) & 0b11) == 0); // Preloads from accumulator not supported
        }

        // In OS mode, pe_state stores the accumulator values
        // In WS mode, pe_state stores the persistent weight matrix
        auto preload_value = (~gemmini_state.preload_sp_addr == 0) ? 0 :
                gemmini_state.spad->at(gemmini_state.preload_sp_addr + i).at(j);
        gemmini_state.pe_state->at(i).at(j) = preload_value;

        dprintf("%d ", gemmini_state.pe_state->at(i).at(j));
      }
      dprintf("\n");
    }
  }

  // Compute
  // For OS, accumulate the PE results internally in pe_state
  // For WS, allocate a new results array which won't affect pe_state, seed the results array with the bias (D) matrix
  auto results = new std::vector<std::vector<accum_t>>(dim, std::vector<accum_t>(dim));
  for (size_t i = 0; i < dim; ++i) {
    for (size_t j = 0; j < dim; ++j) {
      results->at(i).at(j) = (~bd_addr_real == 0) ? 0 : gemmini_state.spad->at(bd_addr_real + i).at(j);
    }
  }
  for (size_t i = 0; i < dim; ++i) {
    for (size_t j = 0; j < dim; ++j) {
      for (size_t k = 0; k < dim; ++k) {
        if (gemmini_state.mode == gemmini_state_t::WS) {
          results->at(i).at(j) += gemmini_state.spad->at(a_addr_real + i).at(k) * gemmini_state.pe_state->at(k).at(j);
        } else {
          gemmini_state.pe_state->at(i).at(j) +=
                  gemmini_state.spad->at(a_addr_real + i).at(k) * gemmini_state.spad->at(bd_addr_real + k).at(j);
        }
      }
    }
  }

  dprintf("GEMMINI: compute - PEs after matmul:\n");
  for (size_t i = 0; i < dim; ++i) {
    for (size_t j = 0; j < dim; ++j) {
      dprintf("%d ", gemmini_state.pe_state->at(i).at(j));
    }
    dprintf("\n");
  }

  // Write results
  if (~gemmini_state.output_sp_addr != 0) {
    bool const acc = (((gemmini_state.output_sp_addr >> 31) & 0x1) == 1);
    bool const acc_accum = (((gemmini_state.output_sp_addr >> 30) & 0x1) == 1);
    auto const base_sp_addr = gemmini_state.output_sp_addr & 0x3FFFFFFF;
    dprintf("GEMMINI: compute - writing results to addr 0x%08x, :\n", gemmini_state.output_sp_addr);

    for (size_t i = 0; i < dim; ++i) {
      for (size_t j = 0; j < dim; ++j) {
        accum_t value = gemmini_state.mode == gemmini_state_t::OS ? gemmini_state.pe_state->at(i).at(j) : results->at(i).at(j);
        if (acc) {
          output_t shifted = gemmini_state.mode == gemmini_state_t::OS ?
                  rounding_saturating_shift<output_t>(value, gemmini_state.sys_shift) :
                  rounding_saturating_shift<output_t>(value, 0);
          if (acc_accum) {
            gemmini_state.accumulator->at(base_sp_addr + i).at(j) += shifted;
          } else { // Overwrite
            gemmini_state.accumulator->at(base_sp_addr + i).at(j) = shifted;
          }
          dprintf("%d ", gemmini_state.accumulator->at(base_sp_addr + i).at(j));
        } else { // Move to scratchpad, apply activation along the way
          input_t shifted = gemmini_state.mode == gemmini_state_t::OS ?
                             rounding_saturating_shift<input_t>(value, gemmini_state.sys_shift) :
                             rounding_saturating_shift<input_t>(value, 0);
          input_t activated = apply_activation(shifted);
          gemmini_state.spad->at(base_sp_addr + i).at(j) = activated;
          dprintf("%d ", gemmini_state.spad->at(base_sp_addr + i).at(j));
        }
      }
      dprintf("\n");
    }
  }
}

void gemmini_t::loop_ws(reg_t rs1, reg_t rs2) {
  const auto A_sp_addr_start = static_cast<uint32_t>(rs1 & 0xFFFFFFFF);
  const auto B_sp_addr_start = static_cast<uint32_t>((rs1 >> 32) & 0xFFFFFFFF);
  uint32_t C_sp_addr_start = 3 << (addr_len - 2);

  const auto I = static_cast<uint32_t>(rs2 & 0xFFFF);
  const auto J = static_cast<uint32_t>((rs2 >> 16) & 0xFFFF);
  const auto K = static_cast<uint32_t>((rs2 >> 32) & 0xFFFF);

  const auto bias = static_cast<bool>((rs2 >> 48) & 0x1);

  const uint32_t garbage_addr = ~0;

  for (size_t j = 0; j < J; j++) {
    for (size_t k = 0; k < K; k++) {
      const uint32_t B_sp_addr = B_sp_addr_start + (k * J + j) * dim;

      for (size_t i = 0; i < I; i++) {
        const uint32_t A_sp_addr = A_sp_addr_start + (i*K + k)*dim;
        const uint32_t C_sp_addr = C_sp_addr_start + (i*J + j)*dim;

        const uint32_t pre_sp_addr = i == 0 ? B_sp_addr : garbage_addr;
        uint32_t out_sp_addr = C_sp_addr;

        int no_bias_new_matrix = !bias && k == 0;
        if (no_bias_new_matrix) {
          out_sp_addr &= ~(1 << (addr_len-2));
        }

        preload(pre_sp_addr, out_sp_addr);
        compute(A_sp_addr, garbage_addr, i == 0);
      }
    }
  }
}

// EE290DAYEOL
void gemmini_t::lock() {
	// if user priv	
	printf("Privilege: %d\n", p->get_state()->prv);
	
	if(p->get_state()->prv == 0 || gemmini_state.locked) {
		illegal_instruction();
	} else {
		gemmini_state.locked = 1;
		gemmini_state.locked_satp = p->get_state()->satp;
		printf("Gemmini locked with %x\n",p->get_state()->satp);
	}
}
void gemmini_t::unlock() {
	if(p->get_state()->prv == 0 || !gemmini_state.locked) {
		illegal_instruction();
	} else {
		gemmini_state.locked = 0;
		gemmini_state.locked_satp = 0;
		printf("Gemmini unlocked\n");
	}
}

reg_t gemmini_t::custom3(rocc_insn_t insn, reg_t xs1, reg_t xs2) {
	// EE290DAYEOL
	if (gemmini_state.locked == 1 && 
			gemmini_state.locked_satp != p->get_state()->satp) {
		illegal_instruction();
	}

  if (insn.funct == mvin_funct)
    mvin(xs1, xs2);
  else if (insn.funct == mvout_funct)
    mvout(xs1, xs2);
  else if (insn.funct == preload_funct)
    preload(xs1, xs2);
  else if (insn.funct == setmode_funct)
    setmode(xs1, xs2);
  else if (insn.funct == compute_preloaded_funct)
    compute(xs1, xs2, true);
  else if (insn.funct == compute_accumulated_funct)
    compute(xs1, xs2, false);
  else if (insn.funct == flush_funct) {
    dprintf("GEMMINI: flush\n");
  }
  else if (insn.funct == loop_ws_funct) {
    loop_ws(xs1, xs2);
  } 
	// EE290DAYEOL
	else if (insn.funct == lock_funct) {
		lock();
  } 
	else if (insn.funct == unlock_funct) {
		unlock();
  }
	else {
    dprintf("GEMMINI: encountered unknown instruction with funct: %d\n", insn.funct);
    illegal_instruction();
  }
  return 0;
}

// Applying activation from PE post-shifted output to scratchpad (for OS dataflow)
// or from accumulator to DRAM (after shifting, for WS dataflow)
input_t gemmini_t::apply_activation(input_t value) {
  if (gemmini_state.act == gemmini_state_t::RELU) {
    return value > 0 ? static_cast<input_t>(value) : static_cast<input_t>(0);
  } else if (gemmini_state.act == gemmini_state_t::RELU6) {
    auto positive = value > 0 ? value : static_cast<input_t>(0);
    return value > (6 << gemmini_state.relu6_shift) ? static_cast<input_t>(6 << gemmini_state.relu6_shift) : positive;
  } else if (gemmini_state.act == gemmini_state_t::NONE) {
    return static_cast<input_t>(value);
  } else assert(false);
}

template <class T>
T gemmini_t::rounding_saturating_shift(accum_t value, uint64_t shift) {
  // Rounding right shift equation: https://riscv.github.io/documents/riscv-v-spec/#_vector_fixed_point_rounding_mode_register_vxrm
  int r = (shift == 0 ? 0 : ((value >> (shift-1)) & 1)) &
       (((shift <= 1 ? 0 : (value & ((1 << (shift-1)) - 1))) != 0) | ((value >> shift) & 1));
  accum_t shifted = (value >> shift) + r;

  // Saturate and cast element
  auto elem_t_max = std::numeric_limits<T>::max();
  auto elem_t_min = std::numeric_limits<T>::min();
  int64_t elem = shifted > elem_t_max ? elem_t_max : (shifted < elem_t_min ? elem_t_min : shifted);
  return static_cast<T>(elem);
}
