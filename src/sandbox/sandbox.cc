#include "src/sandbox/sandbox.h"

#include <cassert>
#include <setjmp.h>
#include <signal.h>

#include <algorithm>

using namespace std;
using namespace stoke;
using namespace x64asm;

namespace {

sigjmp_buf buf_;

void sigfpe_handler(int signum, siginfo_t* si, void* data) {
	siglongjmp(buf_, 1);
}

void callback_wrapper(StateCallback cb, size_t line, CpuState* current, void* arg) {
  cb({line, *current}, arg);
}

} // namespace

namespace stoke {

Sandbox::Sandbox() : fxn_(32*1024) {
	clear_inputs();
	clear_callbacks();

  set_max_jumps(1024);
  set_read_only_mem(false);

  snapshot_.init();
	segv_buffer_.fill(0);

	static bool once = false;
	if ( !once ) {
		once = true; 

		struct sigaction sa;
		sa.sa_sigaction = sigfpe_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART | SA_ONSTACK | SA_SIGINFO | SA_NODEFER;

		const auto res = sigaction(SIGFPE, &sa, 0); (void) res;
		assert(res != -1 && "Unable to install sigfpe handler!");
	}
}

Sandbox& Sandbox::clear_inputs() {
	for ( auto io : io_pairs_ ) {
		delete io;
	}
  io_pairs_.clear();
	return *this;
}

Sandbox& Sandbox::insert_input(const CpuState& input) {
  io_pairs_.push_back(new IoPair());
	auto io = io_pairs_.back();

  // Use this input as both input AND output
  io->in_ = input;
  io->out_ = input;

  // Note that we never copy rsp TO the cpu. This has nasty function
  // call return implications, which we have to handle at call point.
  // We handle rsp separately as part of the user's callee-saved state.

	// Assemble helper functions for this io pair.
	io->in2cpu_ = CpuIo::write(io->in_, RegSet::universe() - (RegSet::empty()+rsp));
	io->out2cpu_ = CpuIo::write(io->out_, RegSet::universe() - (RegSet::empty()+rax+rsp));
	io->cpu2out_ = CpuIo::read(io->out_, RegSet::universe(), {{rax,&scratch_[rax]}, {rsp,&scratch_[rsp]}});
	io->map_addr_ = assemble_map_addr(io->out_);

	return *this;
}

/** The goal here is to assemble a reusable function for every in_[i]. We can 
  assume that globally visible current_xxx vars will be defined appropriately. */
void Sandbox::compile(const Cfg& cfg) {
  const auto& code = cfg.get_code();
	assm_.start(fxn_);

  emit_save_stoke_callee_save();
	assm_.call(rdi);
  assm_.mov(rsp, Imm64{&current_frame_});
  assm_.mov(rsp, M64{rsp});
  emit_save_user_callee_save();

	// Assemble reachable blocks
	for ( auto b = cfg.reachable_begin(), be = cfg.reachable_end(); b != be; ++b ) {
		const auto begin = cfg.get_index(Cfg::loc_type(*b,0));
		for ( size_t i = begin, ie = begin + cfg.num_instrs(*b); i < ie; ++i ) {
			const auto& instr = code[i];

			if ( !before_.empty() ) {
        emit_callbacks(i, true);
      }
      if ( instr.is_jump() ) {
        emit_pre_jump();        
      }
			if ( instr.is_return() ) {
        emit_pre_return();
      }
      emit_instruction(instr);
			if ( !after_.empty() )  {
        emit_callbacks(i, false); 
      }
		}
	}
  emit_infinite_loop_return();

	assm_.finish();
}

void Sandbox::run_all() {
	for ( size_t i = 0, ie = size(); i < ie; ++i )
		run_one(i);
}

void Sandbox::run_one(size_t index) {
  assert(index < size());
  auto io = io_pairs_[index];

  // Reset infinite loop and segfault state
  jumps_ = max_jumps_;
  segv_ = 0;

  // Set the global variables for this input
  current_frame_ = io->in_.gp[rsp].get_fixed_quad(0);
  current_state_ = (uint64_t)&io->out_;
  current_c2o_ = (uint64_t)(io->cpu2out_.get_entrypoint());
  current_o2c_ = (uint64_t)(io->out2cpu_.get_entrypoint());
  current_map_addr_ = (uint64_t)(io->map_addr_.get_entrypoint());

  // In read only mem mode, we don't need to do this; mem will never change
  if ( !read_only_mem_ ) {
		io->out_.stack.copy_defined(io->in_.stack);
		io->out_.heap.copy_defined(io->in_.heap);
		segv_buffer_.fill(0);
  }

 	// Run the code and set signal flags
  if ( !sigsetjmp(buf_, 1) ) {
    fxn_.call<void,void*>(io->in2cpu_.get_entrypoint());

    if ( jumps_ == 0 ) {
      io->out_.code = ErrorCode::SIGKILL_;
    } else if ( !snapshot_.check_abi(io->out_) ) {
      io->out_.code = ErrorCode::SIGSEGV_;
    } else if ( segv_ != 0 ) {
      io->out_.code = ErrorCode::SIGSEGV_;
    } else {
      io->out_.code = ErrorCode::NORMAL;
    }
  } else {
    io->out_.code = ErrorCode::SIGFPE_;
  }
}

void Sandbox::emit_save_stoke_callee_save() {
  // This method is invoked prior to any user code, so rsp is sound.
  // Also invoked prior to loading user input, so no need to save rax

  // Backup rsp where the snapshot is expecting it
  assm_.mov(rax, rsp);
  assm_.mov(Moffs64{snapshot_.get_stoke_rsp()}, rax);
  // Called from a valid rsp context
  assm_.mov((R64)rax, Imm64{snapshot_.get_save_stoke_callee_save()});
  assm_.call(rax);
}

void Sandbox::emit_save_user_callee_save() {
  // Backup rax and save rsp where the snapshot is expecting it
  assm_.mov(Moffs64{&scratch_[rax]}, rax);
  assm_.mov(rax, rsp);
  assm_.mov(Moffs64{snapshot_.get_user_rsp()}, rax);
  // Load the real stack pointer 
  assm_.mov(rax, Moffs64{snapshot_.get_stoke_rsp()});
  assm_.mov(rsp, rax);
  // Called from a valid rsp context
  assm_.mov((R64)rax, Imm64{snapshot_.get_save_user_callee_save()});
  assm_.call(rax);
  // Restore rax and rsp
  assm_.mov(rax, Moffs64{snapshot_.get_user_rsp()});
  assm_.mov(rsp, rax);
  assm_.mov(rax, Moffs64{&scratch_[rax]});
}

void Sandbox::emit_callbacks(size_t line, bool before) {
  // This won't ever be called on the critical path
  // No need to think too hard about efficiency

  const auto& cbs = before ? before_[line] : after_[line];
  for ( const auto& cb : cbs ) {
    // Save rax and rsp for the read cpu call below
    assm_.mov(Moffs64{&scratch_[rax]}, rax);
    assm_.mov(rax, rsp);
    assm_.mov(Moffs64{&scratch_[rsp]}, rax);
    // Load the real stack pointer 
    assm_.mov(rax, Moffs64{snapshot_.get_stoke_rsp()});
    assm_.mov(rsp, rax);
    // Save rflags
    assm_.pushfq();
    // Copy machine state (called from valid rsp, with user rax and rsp saved)
    assm_.mov(rax, Moffs64{&current_c2o_});
    assm_.call(rax);
    // Invoke the callback through the callback wrapper 
    assm_.mov((R64)rdi, Imm64{cb.first});
    assm_.mov(rsi, Imm64{line});
    assm_.mov(rax, Moffs64{&current_state_});
    assm_.mov(rdx, rax);
    assm_.mov(rcx, Imm64{cb.second});
    assm_.mov((R64)rax, Imm64{&callback_wrapper});
    assm_.call(rax);
    // Restore machine state (called from valid rsp, ignores user rax and rsp)
    assm_.mov(rax, Moffs64{&current_o2c_});
    assm_.call(rax);
    // Restore rflags 
    assm_.popfq();
    // Restore the rsp and rax
    assm_.mov(rax, Moffs64{&scratch_[rsp]});
    assm_.mov(rsp, rax);
    assm_.mov(rax, Moffs64{&scratch_[rax]});
  }
}

void Sandbox::emit_pre_jump() {
  // Save rax and rsp
  assm_.mov(Moffs64{&scratch_[rax]}, rax);
  assm_.mov(rax, rsp);
  assm_.mov(Moffs64{&scratch_[rsp]}, rax);
  // Load the real stack pointer 
  assm_.mov(rax, Moffs64{snapshot_.get_stoke_rsp()});
  assm_.mov(rsp, rax);
  // Save rflags
  assm_.pushfq();

  // Decrement the jump counter
  assm_.mov((R64)rax, Imm64{&jumps_});
  assm_.dec(M64{rax});
  // If we've hit zero, control continues in emit_inifinte_loop_return()
  assm_.je(Label{"infinite_loop"});

  // Restore rflags
  assm_.popfq();
  // Restore rsp and rax
  assm_.mov(rax, Moffs64{&scratch_[rsp]});
  assm_.mov(rsp, rax);
  assm_.mov(rax, Moffs64{&scratch_[rax]});
}

void Sandbox::emit_pre_return() {
  // Backup the value of rax and rsp for the read cpu call
  assm_.mov(Moffs64{&scratch_[rax]}, rax);
  assm_.mov(rax, rsp);
  assm_.mov(Moffs64{&scratch_[rsp]}, rax);
  // Reload the real stack pointer 
  assm_.mov(rax, Moffs64{snapshot_.get_stoke_rsp()});
  assm_.mov(rsp, rax);
  // Save the final state (called from valid rsp, with user rax and rsp saved)
  assm_.mov(rax, Moffs64{&current_c2o_});
  assm_.call(rax);
  // Restore real callee saved registers 
  // (called from valid state and lets us keep real rsp which we just set)
  assm_.mov((R64)rax, Imm64{snapshot_.get_restore_stoke_callee_save()});
  assm_.call(rax);
}

void Sandbox::emit_instruction(const Instruction& instr) {
  const auto rs = instr.maybe_read_set();
  const auto ws = instr.maybe_write_set();

  // Heavy lifting if this instruction dereferences memory
  if ( instr.derefs_mem() ) {
    // No sense in making a copy if we can promise to be const
    const auto mi = instr.mem_index(); 
    auto* temp = const_cast<Instruction*>(&instr);
    const auto old_op = temp->get_operand<M64>(mi);

    // Backup the value of rax 
    assm_.mov(Moffs64{&scratch_[rax]}, rax);
    // Quickly now, before we touch ANYTHING, save the effective address
    assm_.lea(rax, old_op);                      
    assm_.mov(Moffs64{&scratch_[rdi]}, rax);
    // Backup the value of rsp 
    assm_.mov(rax, rsp);
    assm_.mov(Moffs64{&scratch_[rsp]}, rax);
    // Reload the real stack pointer 
    assm_.mov(rax, Moffs64{snapshot_.get_stoke_rsp()});
    assm_.mov(rsp, rax);
    // Save rcx, rdx, rdi, rsi, and rflags
    assm_.push(rcx);
    assm_.push(rdx);
    assm_.push(rdi);
    assm_.push(rsi);
    assm_.pushfq();

    // Load the mapping function into rax, the address to map into rdi, 
    // the alignment mask into rsi, and the read/write mask into rdx/rcx
    // The result will replace rdi.
    assm_.mov(rax, Moffs64{&current_map_addr_});
    switch ( instr.type(mi) ) {
      case Type::M_256:
        assm_.mov(rsi, Imm64{0xffffffffffffffe0});
        assm_.mov(rdx, Imm64{0x00000000ffffffff});
        break;
      case Type::M_128:
        assm_.mov(rsi, Imm64{0xfffffffffffffff0});
        assm_.mov(rdx, Imm64{0x000000000000ffff});
        break;
      case Type::M_64:
        assm_.mov(rsi, Imm64{0xffffffffffffffff});
        assm_.mov(rdx, Imm64{0x00000000000000ff});
        break;
      case Type::M_32:
        assm_.mov(rsi, Imm64{0xffffffffffffffff});
        assm_.mov(rdx, Imm64{0x000000000000000f});
        break;
      case Type::M_16:
        assm_.mov(rsi, Imm64{0xffffffffffffffff});
        assm_.mov(rdx, Imm64{0x0000000000000003});
        break;
      case Type::M_8:
        assm_.mov(rsi, Imm64{0xffffffffffffffff});
        assm_.mov(rdx, Imm64{0x0000000000000001});
        break;
      default:
        assert(false);
        break;
    }
    if ( instr.maybe_write(mi) )
      assm_.mov(rcx, rdx);
    else
      assm_.mov(rcx, Imm64{0});
    if ( !instr.maybe_read(mi) )
      assm_.mov(rdx, Imm64{0});

    assm_.mov(rdi, Imm64{&scratch_[rdi]});
    assm_.mov(rdi, M64{rdi});
    assm_.call(rax);
    // Find a free register to hold the sandboxed address
    size_t idx;
    for ( idx = 8; idx < 16 && rs.contains(r64s[idx]); ++idx );
    assert(idx < 16);
    const auto rx = r64s[idx];
    // Save rx if necessary
    if ( !ws.contains(rx) ) {
      assm_.mov(rax, rx);
      assm_.mov(Moffs64{&scratch_[rx]}, rax);
    }
    // Put the result into rx
    assm_.mov(rx, rdi);

    // Restore rflags, rsi, rdi, rdx, and rcx
    assm_.popfq();
    assm_.pop(rsi);
    assm_.pop(rdi);
    assm_.pop(rdx);
    assm_.pop(rcx);
    // Restore rsp and rax
    assm_.mov(rax, Moffs64{&scratch_[rsp]});
    assm_.mov(rsp, rax);
    assm_.mov(rax, Moffs64{&scratch_[rax]});

    // Assemble the instruction
    temp->set_operand(mi, M8{rx});
    assm_.assemble(*temp);
    temp->set_operand(mi, old_op);
    // Restore rx if necessary
    if ( !ws.contains(rx) ) {
      assm_.mov(rx, Imm64{&scratch_[rx]});
      assm_.mov(rx, M64{rx});
    }

    return;
  }

  // @todo: We should find an elegant way to handle instructions with
  // implicit memory operands. For now, we just replace special cases
  // with their moral equivalents and otherwise emit the raw instruction

  switch ( instr.get_opcode() ) {
    case PUSH_R64:
      emit_instruction(Instruction{LEA_R64_M64, {rsp, M64{rsp, Imm32{(uint32_t)-8}}}});
      emit_instruction(Instruction{MOV_M64_R64, {M64{rsp}, instr.get_operand<R64>(0)}});
      break;
    case POP_R64:
      emit_instruction(Instruction{MOV_R64_M64, {instr.get_operand<R64>(0), M64{rsp}}});
      emit_instruction(Instruction{LEA_R64_M64, {rsp, M64{rsp, Imm32{8}}}});
      break;

    default:
      assm_.assemble(instr);
  }
}

void Sandbox::emit_infinite_loop_return() {
  // Control reaches here from emit_pre_jump().
  assm_.bind(Label{"infinite_loop"});

  // This is identical to the cleanup half of emit_pre_jump

  // Restore rflags
  assm_.popfq();
  // Restore rsp and rax
  assm_.mov(rax, Moffs64{&scratch_[rsp]});
  assm_.mov(rsp, rax);
  assm_.mov(rax, Moffs64{&scratch_[rax]});

  emit_pre_return();
  assm_.ret();
}

Function Sandbox::assemble_map_addr(CpuState& cs) {
  // Input/Output: rdi = the address to sandbox
  // Input: rsi = alignment mask to check against
  // Input: rdx = byte read mask
  // Input: rcx = byte write mask
  // This isn't strict x64 abi, it simulates pass by reference
  // Scratch: rax is safe to use here without restoring its value

  Function fxn;
  assm_.start(fxn);

  // Check alignment: A well aligned address won't change
  // Once this step is done, we can reclaim the space in rsi
  assm_.and_(rsi, rdi);
  assm_.cmp(rsi, rdi);
  assm_.jne(Label{"fail"});

  // Check whether this address is stack or heap
  assm_.mov((R64)rax, Imm64{cs.stack.lower_bound()});
  assm_.cmp(rdi, rax);
  assm_.jl(Label{"heap_case"});

  // Stack case: Check that this address is inside the stack
  // Try mapping this address into the stack
  assm_.sub(rdi, rax);
  // Check that this address isn't above the top of the stack sandbox
  assm_.mov((R64)rax, Imm64{cs.stack.size()});
  assm_.cmp(rdi, rax);
  assm_.jge(Label{"fail"});
  // This address is at least in range, move on to harder checks:
  emit_stack_heap_cases(cs, true);

  // Heap case: Check that the address is inside the heap
  assm_.bind(Label{"heap_case"});
  // Load the bottom of the heap into rax
  assm_.mov((R64)rax, Imm64{cs.heap.lower_bound()}); 
  // Check that this address isn't below the heap sandbox
  assm_.cmp(rdi, rax);
  assm_.jl(Label{"fail"});
  // Map the address into the heap sandbox
  assm_.sub(rdi, rax);
  // Check that this address isn't above the heap sandbox
  assm_.mov((R64)rax, Imm64{cs.heap.size()});
  assm_.cmp(rdi, rax);
  assm_.jge(Label{"fail"});
  // This address is at least in range, move on to harder checks:
  emit_stack_heap_cases(cs, false);

  // ... 
  // The code emitted by emit_stack_heap_cases() will jump to one
  // of the two labels below
  // ...

  // Failure; set the segv flag and return a valid address
  // This address should be invisible to the user
  assm_.bind(Label{"fail"});
  assm_.mov((R64)rax, Imm64{&segv_});
  assm_.inc(M64{rax});
  assm_.mov((R64)rax, Imm64{segv_buffer_.data()});
  assm_.mov(rdi, rax);

  // Done; get out of here
  assm_.bind(Label{"done"});
  assm_.ret();

  assm_.finish();
  return fxn;
}

void Sandbox::emit_stack_heap_cases(CpuState& cs, bool stack) {
  // This function is called inside map_addr, as above
  // Constant: rdi = an in-range sandboxed address (const)
  // Scratch: rsi doesn't hold anything important anymore
  // Scratch: rax is safe to use here without restoring its value
  // Input: rdx = byte read mask
  // Input: rcx = byte write mask

  // Control should jump to either:
  //   done: success after defining the relevant bits and remapping
  //   fail: memory isn't valid, don't define anything

  // Save rcx (we need to use it for the shift instruction below)
  assm_.mov(rax, rcx);
  // We have a valid address, find the nearest 128-bit boundary in the masks
  assm_.mov(rsi, rdi);
  assm_.shr(rsi, Imm8{3});
  // Now shift the byte mask into that region
  assm_.mov(ecx, Imm32{0x07});
  assm_.and_(rcx, rdi);
  assm_.shl(rdx, cl);
  assm_.shl(rax, cl);
  // Restore rcx
  assm_.mov(rcx, rax);
  
  // The read mask shouldn't change when and'ed against the def mask
  if ( stack )
    assm_.mov((R64)rax, Imm64{cs.stack.defined_mask()});
  else
    assm_.mov((R64)rax, Imm64{cs.heap.defined_mask()});
  assm_.mov(rax, M64{rax, rsi, Scale::TIMES_1});
  assm_.and_(rax, rdx);
  assm_.cmp(rax, rdx);
  assm_.jne(Label{"fail"});

  // The write mask shouldn't change when and'ed against the valid mask
  if ( stack )
    assm_.mov((R64)rax, Imm64{cs.stack.valid_mask()});
  else
    assm_.mov((R64)rax, Imm64{cs.heap.valid_mask()});
  assm_.mov(rax, M64{rax, rsi, Scale::TIMES_1});
  assm_.and_(rax, rcx);
  assm_.cmp(rax, rcx);
  assm_.jne(Label{"fail"});

  // Set defined bits
  if ( stack )
    assm_.mov((R64)rax, Imm64{cs.stack.defined_mask()});
  else
    assm_.mov((R64)rax, Imm64{cs.heap.defined_mask()});
  assm_.or_(M64{rax, rsi, Scale::TIMES_1}, rcx);

  // Do final remapping
  if ( stack ) {
    assm_.mov((R64)rax, Imm64{cs.stack.data()});
    assm_.add(rdi, rax);
  } else {
    assm_.mov((R64)rax, Imm64{cs.heap.data()});
    assm_.add(rdi, rax);
  }

  // Get out of here
  assm_.jmp(Label{"done"});
}

} // namespace stoke