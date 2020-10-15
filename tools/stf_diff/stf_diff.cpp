// <stf_diff> -*- C++ -*-

/**
 * \brief  This tool compares two traces
 *
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "dtl/dtl.hpp"
#pragma GCC diagnostic pop

#include "stf_diff.hpp"

#include "print_utils.hpp"
#include "format_utils.hpp"
#include "stf_inst_reader.hpp"
#include "stf_decoder.hpp"

std::ostream& operator<<(std::ostream& os, const STFDiffInst& inst) {
    /*
    if (inst.physPC_ == INVALID_PHYS_ADDR)
        snprintf(buf, 100, "%ld:\t%#016lx", inst.index, inst.pc);
    else
        snprintf(buf, 100, "%ld:\t%#016lx:%#010lx:", inst.index, inst.pc,
                inst.physPC_);
    */

    stf::format_utils::formatDec(os, inst.index_);
    os << ":\t";
    stf::format_utils::formatHex(os, inst.pc_);
    os << " : ";
    stf::format_utils::formatHex(os, inst.opcode_);
    os << ' ';

    inst.dis_->printDisassembly(inst.pc_, inst.isa_, inst.opcode_, os);

    if (!inst.mem_accesses_.empty()) {
        /*if (inst.mem_accesses[0].paddr == INVALID_PHYS_ADDR)
            snprintf(buf, 100, " MEM %#016lx: [",
                    inst.mem_accesses[0].addr);
        else
            snprintf(buf, 100, " MEM %#016lx:%#010lx: [",
                    inst.mem_accesses[0].addr,
                    inst.mem_accesses[0].paddr);*/
        os << " MEM ";
        stf::format_utils::formatHex(os, inst.mem_accesses_.front().getAddress());
        os << ": [";
    }

    for (const auto &mit : inst.mem_accesses_) {
        os << ' ';
        stf::format_utils::formatHex(os, mit.getData());
    }

    if (!inst.mem_accesses_.empty()) {
        os << " ]";
    }

    for (const auto &rit : inst.operands_) {
        os << "   " << rit.getLabel() << ": " << rit.getReg() << " : ";
        stf::format_utils::formatHex(os, rit.getData());
    }

    return os;
}

auto getBeginIterator(const uint64_t start, const bool diff_markpointed_region, const bool diff_tracepointed_region, stf::STFInstReader& reader) {
    auto it = std::next(reader.begin(), static_cast<ssize_t>(start) - 1);

    if(diff_markpointed_region || diff_tracepointed_region) {
        stf::STFDecoder decoder;
        while(it != reader.end()) {
            decoder.decode(it->opcode());
            ++it;
            if((diff_markpointed_region && decoder.isMarkpoint()) || (diff_tracepointed_region && decoder.isTracepoint())) {
                break;
            }
        }
    }

    return it;
}

// Given two traces (and some config info), report the first n differences
// between the two. n == config.diff_count.
int streamingDiff(const STFDiffConfig &config,
                  const std::string &trace1,
                  const std::string &trace2) {
    // Open stf trace reader
    stf::STFInstReader rdr1(trace1);
    stf::STFInstReader rdr2(trace2);

    auto reader1 = getBeginIterator(config.start1, config.diff_markpointed_region, config.diff_tracepointed_region, rdr1);
    auto reader2 = getBeginIterator(config.start2, config.diff_markpointed_region, config.diff_tracepointed_region, rdr2);

    uint64_t count = 0;
    uint64_t diff_count = 0;

    stf::Disassembler dis(config.use_aliases);
    const auto inst_set = rdr1.getISA();
    stf_assert(inst_set == rdr2.getISA(), "Traces must have the same instruction set in order to be compared!");

    while ((!config.length) || (count < config.length)) {
        if (diff_count >= config.diff_count) {
            break;
        }

        if(reader1 == rdr1.end()) {
            if(reader2 == rdr2.end()) {
                break;
            }

            // Print out the stuff from trace 2
            diff_count++;
            if (!config.only_count) {
                const auto& inst2 = *reader2;
                std::cout << "+ "
                          << STFDiffInst(inst2,
                                         config.diff_physical_data,
                                         config.diff_physical_pc,
                                         config.diff_memory && !inst2.getMemoryAccesses().empty(),
                                         config.diff_registers,
                                         config.ignore_addresses,
                                         inst_set,
                                         &dis)
                          << std::endl;
            }
            reader2++;
            count++;
            continue;
        }

        // Check if the second trace ended early
        if (reader2 == rdr2.end()) {
            diff_count++;
            if (!config.only_count) {
                const auto& inst1 = *reader1;
                std::cout << "- "
                          << STFDiffInst(inst1,
                                         config.diff_physical_data,
                                         config.diff_physical_pc,
                                         config.diff_memory && !inst1.getMemoryAccesses().empty(),
                                         config.diff_registers,
                                         config.ignore_addresses,
                                         inst_set,
                                         &dis)
                          << std::endl;
            }
            reader1++;
            count++;
            continue;
        }

        const auto& inst1 = *reader1;
        const auto& inst2 = *reader2;

        // Check on the kernel code
        if (config.ignore_kernel) {
            // Skip any kernel code
            if (inst1.isKernelCode()) {
                reader1++;
                continue;
            }

            if (inst2.isKernelCode()) {
                reader2++;
                continue;
            }
        }

        STFDiffInst diff1(inst1,
                          config.diff_physical_data,
                          config.diff_physical_pc,
                          config.diff_memory && !inst1.getMemoryAccesses().empty(),
                          config.diff_registers,
                          config.ignore_addresses,
                          inst_set,
                          &dis);
        STFDiffInst diff2(inst2,
                          config.diff_physical_data,
                          config.diff_physical_pc,
                          config.diff_memory && !inst2.getMemoryAccesses().empty(),
                          config.diff_registers,
                          config.ignore_addresses,
                          inst_set,
                          &dis);

        if (diff1 != diff2) {
            diff_count++;
            if (!config.only_count) {
                std::cout << "- " << diff1 << std::endl;
                std::cout << "+ " << diff2 << std::endl;
            }
        }

        reader1++;
        reader2++;
        count++;
    }

    if (config.only_count) {
        std::cout << diff_count << " different instructions" << std::endl;
    }

    return !!diff_count;
}

void extractInstructions(const std::string &trace,
                         DiffInstVec &vec,
                         const uint64_t start,
                         const STFDiffConfig &config) {
    // Open stf trace reader
    stf::STFInstReader rdr(trace);
    stf::Disassembler dis(config.use_aliases);
    const auto inst_set = rdr.getISA();
    auto reader = getBeginIterator(start, config.diff_markpointed_region, config.diff_tracepointed_region, rdr);

    uint64_t count = 0;
    uint64_t last_user_pc = 0xffffffffffffffffULL;
    while (reader != rdr.end()) {
        const auto& inst = *reader;

        // Check if we're in kernel
        if (config.ignore_kernel) {
            if (inst.isKernelCode()) {
                reader++;
                continue;
            }

            // We want the replayed instruction after the page
            // fault, not the first one with no memory records
            if (inst.pc() == last_user_pc) {
                vec.pop_back();
                count--;
            }
        }

        vec.emplace_back(inst,
                         config.diff_physical_data,
                         config.diff_physical_pc,
                         config.diff_memory && !inst.getMemoryAccesses().empty(),
                         config.diff_registers,
                         config.ignore_addresses,
                         inst_set,
                         &dis);

        last_user_pc = inst.pc();

        reader++;
        count++;

        // length of 0 means to keep going till we hit the end
        if (config.length && (count >= config.length)) {
            break;
        }
    }
}

int main (int argc, char **argv) {
    int ret = 0;
    try {
        const STFDiffConfig config(argc, argv);

        if (config.unified_diff) {
            DiffInstVec instVec1;
            DiffInstVec instVec2;

            extractInstructions(config.trace1,
                                instVec1,
                                config.start1,
                                config);
            extractInstructions(config.trace2,
                                instVec2,
                                config.start2,
                                config);

            std::cerr << "Now diffing " << instVec1.size() << "(" << instVec2.size() << ") instructions" << std::endl;

            dtl::Diff<STFDiffInst, DiffInstVec> d(instVec1, instVec2);

            d.onHuge();

            d.compose();

            d.composeUnifiedHunks();

            d.printUnifiedFormat();
        }
        else {
            ret = streamingDiff(config, config.trace1, config.trace2);
        }
    }
    catch(const trace_tools::CommandLineParser::EarlyExitException& e) {
        std::cerr << e.what() << std::endl;
        return e.getCode();
    }

    return ret;
}