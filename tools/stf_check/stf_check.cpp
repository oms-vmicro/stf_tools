
// <STF_check> -*- C++ -*-

/**
 * \brief  This tool checks the input trace for known issues.
 *
 */

/*
    This issues that are checked for:

    1. Check for invalid PC values in mode.  PC values must be 32 bit aligned.  (exit code 1)
    2. Check for invalid PC values in mode.  All pc values must have lsb set to 1. (exit code 2)
    3. Verify that TraceInfo record is present (before the first instruction record) and has a valid generator value and version. (exit code 3)
    4. Invalid instruction (exit code 4).
    5. Check accuracy of STF_CONTAIN_PTE feature flag (exit code 5)
        ** This flag has been re-defined to indicate embedded PTEs, PTE headers are no longer allowed (see ERROR 10)
    6. Check for PA == 0 (exit code 6)
    7. Verify that PA & 0xFFF == VA (exit code 7)
    8. Check for inst_phys_pc records with a value of 0xFFFFFFFF (an obsolete value set by early QEMU versions) (exit code 8)
    9. Check accuracy of STF_CONTAIN_PHYSICAL_ADDRESS feature flag (exit code 9)
    10. Check for pte header - none should be present (exit code 10)
    11. Check accuracy of STF_CONTAIN_RV64 feature flag (exit code 11).

    Error code of 255 is returned for multiple errors if '-c' option is specified.

*/

#include <string>
#include <sstream>
#include <cstring>
#include <iomanip>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <utility>

#include "command_line_parser.hpp"
#include "stf_check.hpp"
#include "stf_decoder.hpp"
#include "stf_inst_reader.hpp"
#include "tools_util.hpp"

static STFCheckConfig parse_command_line (int argc, char **argv) {
    STFCheckConfig config;

    trace_tools::CommandLineParser parser("stf_check");
    parser.addFlag('c', "continue on error");
    parser.addFlag('d', "print out memory point to zero errors");
    parser.addFlag('p', "print trace info");
    parser.addFlag('P', "check prefetch memory address");
    parser.addFlag('n', "skip checking for phsyical address");
    parser.addFlag('v', "always print error counts at the end");
    parser.addFlag('e', "M", "end checking at M-th instruction");
    parser.addMultiFlag('i', "err", "ignore the specified error type");
    parser.addPositionalArgument("trace", "trace in STF format");

    parser.parseArguments(argc, argv);
    config.continue_on_error = parser.hasArgument('c');
    config.print_memory_zero_warnings = parser.hasArgument('d');
    config.print_info = parser.hasArgument('p');
    config.check_phys_addr = !parser.hasArgument('n');
    parser.getArgumentValue('e', config.end_inst);
    config.always_print_error_counts = parser.hasArgument('v');
    for(const auto& err: parser.getMultipleValueArgument('i')) {
        config.ignored_errors.insert(parseErrorCode(err));
    }

    parser.getPositionalArgument(0, config.trace_filename);

    return config;
}

inline bool isVLZero(const stf::STFInst& inst) {
    if(inst.isVector()) {
        for(const auto& op: inst.getSourceOperands()) {
            if(op.getReg() == stf::Registers::STF_REG::STF_REG_CSR_VL) {
                if(op.getScalarValue() == 0) {
                    return true;
                }
                return false;
            }
        }
    }
    return false;
}

int main (int argc, char **argv) {
    try {
        uint64_t inst_count = 0;
        uint64_t hdr_pte_count = 0;         // Number of pte entries found in trace header.
        uint64_t embed_pte_count = 0;       // Number of embedded pte entries found in trace.
        uint64_t pa_count = 0;              // Number of mem PA records found in trace.
        uint64_t phys_pc_count = 0;         // Number of inst_phys_pc values in trace.
        uint64_t memAccesspack = 0;         // Number of memory access record could be packed in 8 bytes

        const STFCheckConfig config = parse_command_line (argc, argv);
        ErrorTracker ecount(config.ignored_errors);
        ecount.setContinueOnError(config.continue_on_error);

        // Keep track of thread_info;
        // NOTE: PT-364 - stf_check to work with multi thread traces
        uint32_t tid = 0;
        uint32_t pid = 0;
        uint32_t asid = 0;
        uint32_t tid_prev = std::numeric_limits<uint32_t>::max();
        uint32_t pid_prev = std::numeric_limits<uint32_t>::max();
        uint32_t asid_prev = std::numeric_limits<uint32_t>::max();
        bool thread_switch = false;
        ThreadMap thread_pc_prev;
        ThreadMapKey thread_id;

        // Open stf trace reader
        stf::STFInstReader stf_reader(config.trace_filename);
        stf::STFDecoder decoder(stf_reader.getInitialIEM());
        /* FIXME Because we have not kept up with STF versioning, this is currently broken and must be loosened.
        if (!stf_reader.checkVersion()) {
            exit(1);
        }
        */

        // Count the pte entries in the header.
        /* This check for pte entries in the header is incorrect; Current assumption is that ptes in the header doesn't matter
        for (stf::STFInstReader::pte_iterator it = stf_reader.pteBegin(); it != stf_reader.pteEnd(); it ++) {
            hdr_pte_count++;
        }*/

        const auto it = stf_reader.begin();
        // Get initial trace thread info
        if(it != stf_reader.end()) {
            const auto& inst = *it;
            tid_prev = inst.tid();
            pid_prev = inst.tgid();
            asid_prev = inst.asid();
            thread_id = std::make_tuple(tid_prev, pid_prev, asid_prev);
            thread_pc_prev[thread_id] = inst;
        }

        const auto& trace_features = stf_reader.getTraceFeatures();
        const bool has_rv64_inst = stf_reader.getInitialIEM() == stf::INST_IEM::STF_INST_IEM_RV64;

        for(const auto& info: stf_reader.getTraceInfo()) {
            if (info->getGenerator() != stf::STF_GEN::STF_GEN_RESERVED) {      // Check for a valid header.
                try {
                    std::cout << "Trace generator: " << info->getGenerator() << std::endl;
                }
                catch(const stf::STFException& e) {
                    ecount.countError(ErrorCode::HEADER);
                    ecount.reportError(ErrorCode::HEADER) << e.what() << std::endl;
                }
                std::cout << "Trace generator version: " << info->getVersionString() << std::endl;
                std::cout << "Comment: " << info->getComment() << std::endl;
                std::cout << "Features: ";
                stf::print_utils::printHex(trace_features->getFeatures());
                std::cout << std::endl;
            }
            else {
                ecount.countError(ErrorCode::HEADER);
                ecount.reportError(ErrorCode::HEADER) << "Invalid trace info record found" << std::endl;
            }
        }

        // Iterate through all of the records, checking for known issues.
        for (const auto& inst: stf_reader) {
            inst_count++;
            if (!inst.valid()) {
                ecount.countError(ErrorCode::INVALID_INST);
                auto& msg = ecount.reportError(ErrorCode::INVALID_INST);
                msg << inst.index() << " invalid instruction ";
                stf::format_utils::formatHex(msg, inst.opcode());
                msg << " PC ";
                stf::format_utils::formatVA(msg, inst.pc());
                msg << std::endl;
            }

            tid = inst.tid();
            pid = inst.tgid();
            asid = inst.asid();
            thread_id = std::make_tuple(tid, pid, asid);
            thread_switch = (tid != tid_prev || pid != pid_prev || asid != asid_prev);
            tid_prev = tid;
            pid_prev = pid;
            asid_prev = asid;
            const auto& inst_prev = thread_pc_prev[thread_id];
            decoder.decode(inst_prev.opcode());

            //check if trace has physical address translations
            /*if (check_phys_addr && ((trace_features & STF_CONTAIN_PHYSICAL_ADDRESS) == 0)) {
                stringstream msg;
msg     << "STF_CONTAIN_PHYSICAL_ADDRESS not set, but is required as part of the default tracing configuration";
                ecount.phys_addr_9++;
                reportError(msg.str().c_str(), 9);
            }*/

            const auto& prev_events = inst_prev.getEvents();

            // Check for decoder failures on non-faulting instructions
            if(STF_EXPECT_FALSE(decoder.decodeFailed() && prev_events.empty())) {
                ecount.countError(ErrorCode::DECODER_FAILURE);
                auto& msg = ecount.reportError(ErrorCode::DECODER_FAILURE);
                stf::format_utils::formatDecLeft(msg, inst.index(), MAX_COUNT_LENGTH);
                msg << " Failed to decode instruction." << std::endl;
            }

            //check if inst is_load or is_store and doesn't have memory accesses when it should
            if (STF_EXPECT_FALSE(
                    decoder.isLoad() && // it decodes as a load
                    (!inst_prev.isLoad() || inst_prev.getMemoryReads().empty()) && // but it isn't doing any loads
                    prev_events.empty())) { // and there are no events stopping it from doing a load
                bool found = false;
                // Commenting this out for now since RISC-V doesn't have software prefetches
                /*
                OpcodeList::iterator oit = pldOpcodes.begin();
                for (;oit != pldOpcodes.end(); oit++) {
                    if (oit->decodeOpcode(inst.opcode())) {
                        found = true;
                        break;
                    }
                }
                */
                // Special handling for vector loads
                // If the VL register is 0, the load won't do any accesses (this is allowed by the spec)
                found = isVLZero(inst_prev);

                if(STF_EXPECT_FALSE(!found)) {
                    ecount.countError(ErrorCode::MISS_MEM);
                    ecount.countError(ErrorCode::MISS_MEM_LOAD);
                    auto& msg = ecount.reportError(ErrorCode::MISS_MEM_LOAD);
                    stf::format_utils::formatDecLeft(msg, inst.index(), MAX_COUNT_LENGTH);
                    msg << " Load instruction missing memory access record in stf." << std::endl;
                }
            }
            if (STF_EXPECT_FALSE(
                    decoder.isStore() && // it decodes as a store
                    (!inst_prev.isStore() || inst_prev.getMemoryWrites().empty()) && // but it isn't doing any stores
                    prev_events.empty() && // and there are no events stopping it from doing a store
                    !decoder.isAtomic())) { // and this isn't an atomic inst (store-conditional)
                bool found = false;
                // Commenting this out for now since RISC-V doesn't have software prefetches
                /*
                OpcodeList::iterator oit = pldOpcodes.begin();
                for (;oit != pldOpcodes.end(); oit++) {
                    if (oit->decodeOpcode(inst.opcode())) {
                        found = true;
                        break;
                    }
                }
                */
                // Special handling for vector stores
                // If the VL register is 0, the store won't do any accesses (this is allowed by the spec)
                found = isVLZero(inst_prev);

                if (!found) {
                    ecount.countError(ErrorCode::MISS_MEM);
                    ecount.countError(ErrorCode::MISS_MEM_STR);
                    auto& msg = ecount.reportError(ErrorCode::MISS_MEM_STR);
                    stf::format_utils::formatDecLeft(msg, inst.index(), MAX_COUNT_LENGTH);
                    msg << " Store instruction missing memory access record in stf." << std::endl;
                }
            }

            if (STF_EXPECT_FALSE(config.end_inst && (inst.index() > config.end_inst))) {
                break;
            }

            if(STF_EXPECT_FALSE((inst.pc() & 1) != 0)) {
                if(inst.isOpcode16()) {
                    ecount.countError(ErrorCode::INVALID_PC_16);
                    auto& msg = ecount.reportError(ErrorCode::INVALID_PC_16);
                    msg << "Invalid pc value found in mode at instruction #";
                    stf::format_utils::formatDec(msg, inst.index());
                    msg << " pc value: ";
                    stf::format_utils::formatVA(msg, inst.pc());
                    msg << std::endl;
                }
                else {
                    ecount.countError(ErrorCode::INVALID_PC_32);
                    auto& msg = ecount.reportError(ErrorCode::INVALID_PC_32);
                    msg << "Invalid pc value found in mode at instruction #";
                    stf::format_utils::formatDec(msg, inst.index());
                    msg << " pc value: ";
                    stf::format_utils::formatVA(msg, inst.pc());
                    msg << std::endl;
                }
            }

            size_t mem_access_size = 0;
            const auto& mem_accesses = inst.getMemoryAccesses();
            // Check for physcial address issues in memory access records.
            for(const auto& mem_access: mem_accesses) {
                mem_access_size += mem_access.getSize();
                // Check if accesses address zero
                if (STF_EXPECT_FALSE(mem_access.getAddress() == 0)) {
                    ecount.countError(ErrorCode::MEM_POINT_TO_ZERO);
                    if (config.print_memory_zero_warnings) {
                        auto& msg = ecount.reportError(ErrorCode::MEM_POINT_TO_ZERO);
                        msg << "Instruction memory record points to vaddr 0 at instruction #";
                        stf::format_utils::formatDec(msg, inst.index());
                        msg << " pc value: ";
                        stf::format_utils::formatVA(msg, inst.pc());
                        msg << std::endl;
                    }
                }
                if (STF_EXPECT_FALSE(mem_access.getAttr() == 0)) {
                    ecount.countError(ErrorCode::MEM_ATTR);
                    auto& msg = ecount.reportError(ErrorCode::MEM_ATTR);
                    msg << "The Instruction accesses memory. But there is no memory access attribute record at instruction index " << inst.index() << std::endl;
                }

                // Commenting this out for now until we get translation figured out
#if 0
                // Check to see if paddr is valid.

                if (mem_access.isPhysAddrValid()) {
                    pa_count++;

                    // Verify paddr is not 0.
                    if (mem_access.paddr == 0) {
                        // check if the instruction is PLD instr;
                        bool isPLDInstr = false;
                        // Commenting this out for now since RISC-V doesn't have software prefetches
                        /*
                        OpcodeList::iterator oit = pldOpcodes.begin();
                        for (;oit != pldOpcodes.end(); oit++) {
                            if (oit->decodeOpcode(inst.opcode())) {
                                isPLDInstr = true;
                                break;
                            }
                        }
                        */
                        if (!isPLDInstr) {
                            std::stringstream msg;
                            msg << "PA == 0 at instruction index " << std::dec << inst.index();
                            std::cerr << "stf_check: WARNING " << std::dec << 6 << ": " << msg.str().c_str() << std::endl;   // Send message to stderr.
                            return_code = 6;
                            ecount.pa_eq_0_6++;
                            //print out error but don't exit or send exit code
                            //reportError(msg.str().c_str(), 6);
                        }
                    } else if ((mem_access.paddr & 0xfff) != (mem_access.addr & 0xfff)) {
                        std::stringstream msg;
                        msg << "PA & 0xfff != VA & 0xfff at instruction index " << std::dec << inst.index();
                        ecount.pa_ne_va_7++;
                        reportError(msg.str().c_str(), 7);
                    }
                }
#endif
            }

            if ((mem_access_size <= 4) && (mem_accesses.size() > 1)) {
                memAccesspack++;
            }


            // Commenting this out until we get translation figured out
            /*
            // Check for invalid inst_phys_pc value.
            if (inst.physPc() == 0xffffffff) {
                // Scan all event entries to check if this instruction was aborted due to a code page fault.
                bool bPrefetchAbort = false;

                if (inst.numEvents() > 0) {
                    for (stf::STFInst::event_const_iterator eit = inst.eventBegin(); eit != inst.eventEnd(); eit++) {
                        if ((*eit).event == STF_EVENT_PREFETCH_ABORT) {
                            bPrefetchAbort = true;
                        }

                    }
                }

                // If this was a code page fault, then ignore any PC errors.
                if (!bPrefetchAbort) {
                    std::stringstream msg;
                    msg << "Invalid phys PC value at index " << std::dec << inst.index();
                    ecount.invalid_phys_8++;
                    reportError(msg.str().c_str(), 8);
                }
            } else if (inst.physPc() != 0xffffffffffffffff) {
                phys_pc_count++;
            }
            */

            // Check for embedded PTEs
            embed_pte_count += inst.getEmbeddedPTEs().size();

            // check if unconditional branch contains PC_TARGET
            // If last instruction is an unconditional branch, it will not have PC TARGET.
            if (STF_EXPECT_TRUE(!thread_switch)) {
                if (STF_EXPECT_FALSE(decoder.isBranch() && !decoder.isConditional())) {
                    if (STF_EXPECT_FALSE(prev_events.empty() && (!inst_prev.isTakenBranch()))) {
                        ecount.countError(ErrorCode::UNCOND_BR);
                        auto& msg = ecount.reportError(ErrorCode::UNCOND_BR);
                        stf::format_utils::formatDecLeft(msg, inst_prev.index(), MAX_COUNT_LENGTH);
                        msg << " 0x";
                        stf::format_utils::formatVA(msg, inst_prev.pc());
                        msg << " Unconditional Branch instr does not have PC Target(no event)." << std::endl;
                    }
                }

                // if switch to user mode; check if previous instruction is sret or mret;
                const bool is_mode_change_to_user = inst_prev.isChangeToUserMode();

                if (STF_EXPECT_FALSE(is_mode_change_to_user && !decoder.isExceptionReturn())) {
                    ecount.countError(ErrorCode::SWITCH_USR);
                    auto& msg = ecount.reportError(ErrorCode::SWITCH_USR);
                    stf::format_utils::formatDecLeft(msg, inst_prev.index(), MAX_COUNT_LENGTH);
                    msg << " 0x";
                    stf::format_utils::formatVA(msg, inst_prev.pc());
                    msg << " Switch to user mode ";
                    stf::format_utils::formatDec(msg, inst.index(), MAX_COUNT_LENGTH);
                    msg << " 0x";
                    stf::format_utils::formatVA(msg, inst.pc());
                    msg << " without sret/mret/instr." << std::endl;
                }
            }

            thread_pc_prev[thread_id] = inst;
        }

        // Check to see if the STF_CONTAINS_PHYSCICAL_ADDRESS flag is set properly.
        if (trace_features->hasFeature(stf::TRACE_FEATURES::STF_CONTAIN_PHYSICAL_ADDRESS)) {
            if (pa_count == 0) {
                ecount.countError(ErrorCode::PHYS_ADDR);
                ecount.reportError(ErrorCode::PHYS_ADDR) << "STF_CONTAIN_PHYSICAL_ADDRESS set, but no PAs found" << std::endl;
            }
            if (phys_pc_count == 0) {
                ecount.countError(ErrorCode::PHYS_ADDR);
                ecount.reportError(ErrorCode::PHYS_ADDR) << "STF_CONTAIN_PHYSICAL_ADDRESS set, but no INST PHYS PC found" << std::endl;
            }
        }
        else {
            if (pa_count > 0) {
                ecount.countError(ErrorCode::PHYS_ADDR);
                auto& msg = ecount.reportError(ErrorCode::PHYS_ADDR);
                msg << "STF_CONTAIN_PHYSICAL_ADDRESS not set, but ";
                stf::format_utils::formatDec(msg, pa_count);
                msg << " PAs are present" << std::endl;
            }
            if (phys_pc_count > 0) {
                ecount.countError(ErrorCode::PHYS_ADDR);
                auto& msg = ecount.reportError(ErrorCode::PHYS_ADDR);
                msg << "STF_CONTAIN_PHYSICAL_ADDRESS not set, but ";
                stf::format_utils::formatDec(msg, phys_pc_count);
                msg << " INST PHYS PCs are present" << std::endl;
            }
        }

        // Check to see if pte table is found.
        if (hdr_pte_count > 0) {
            ecount.countError(ErrorCode::PTE_HEADER);
            ecount.reportError(ErrorCode::PTE_HEADER) << "PTE Values found in header" << std::endl;
        }

        // Check to see if the STF_CONTAIN_PTE flag is correct.
        if (trace_features->hasFeature(stf::TRACE_FEATURES::STF_CONTAIN_PTE)) {
            if (embed_pte_count == 0) {
                ecount.countError(ErrorCode::EMBED_PTE);
                ecount.reportError(ErrorCode::EMBED_PTE) << "STF_CONTAIN_PTE set, but no PTEs found" << std::endl;
            }
        } else if (embed_pte_count > 0) {
            ecount.countError(ErrorCode::EMBED_PTE);
            auto& msg = ecount.reportError(ErrorCode::EMBED_PTE);
            msg << "STF_CONTAIN_PTE not set, but ";
            stf::format_utils::formatDec(msg, embed_pte_count);
            msg << " PTEs are present" << std::endl;
        }

        // Check to see if the STF_CONTAIN_RV64 feature is set correctly.
        if (has_rv64_inst && !trace_features->hasFeature(stf::TRACE_FEATURES::STF_CONTAIN_RV64)) {
            ecount.countError(ErrorCode::RV64_INSTS);
            auto& msg = ecount.reportError(ErrorCode::RV64_INSTS);
            msg << "STF_CONTAIN_RV64 not set, but RV64 instructions are present" << std::endl;
        }
        else if (!has_rv64_inst && trace_features->hasFeature(stf::TRACE_FEATURES::STF_CONTAIN_RV64)) {
            ecount.countError(ErrorCode::RV64_INSTS);
            ecount.reportError(ErrorCode::RV64_INSTS) << "STF_CONTAIN_RV64 set, but no RV64 instructions are present" << std::endl;
        }

        if (memAccesspack) {
            ecount.countError(ErrorCode::INEFF_MEM_ACC);
            auto& msg = ecount.reportError(ErrorCode::INEFF_MEM_ACC);
            msg << "There are ";
            stf::format_utils::formatDec(msg, memAccesspack);
            msg << " instructions that have inefficient memory access record." << std::endl;
        }

        // Print a summary of findings.
        if (config.print_info) {
            stf::print_utils::printDec(embed_pte_count);
            std::cout << " Embedded PTE entries found" << std::endl;
            std::cout << pa_count << " Physical data addresses found" << std::endl;
            std::cout << phys_pc_count << " Physical instruction addresses found" << std::endl;

        }

        if (!ecount.continueOnError()) {
            static constexpr uint64_t TO_PERCENT = 100;
            const uint64_t mem_point_to_zero = ecount.getErrorCount(ErrorCode::MEM_POINT_TO_ZERO);
            if (mem_point_to_zero > 0) {
                const double pct2 = static_cast<double>(TO_PERCENT * mem_point_to_zero) / static_cast<double>(inst_count);
                std::cout << "Warning: " << mem_point_to_zero << " Memory records point to virtual address zero (" << pct2 << "%)\n";
            }
        }

        if (ecount.getReturnCode() == ErrorCode::PA_EQ_0) {
            std::cout << "Trace file had PA == 0 warning" << std::endl;
            // Currently this is an acceptable error and should not return a failing exit code
            ecount.setReturnCode(ErrorCode::RESERVED_NO_ERROR);
        }

        if (ecount.getReturnCode() == ErrorCode::RESERVED_NO_ERROR) {
            std::cout << "Trace file passed all consistency checks" << std::endl;
        }

        if (ecount.continueOnError() || config.always_print_error_counts) {
            // Print the error counts
            ecount.printErrorCounts();
        }

        return static_cast<int>(ecount.getReturnCode());
    }
    catch(const trace_tools::CommandLineParser::EarlyExitException& e) {
        std::cerr << e.what() << std::endl;
        return e.getCode();
    }
}
