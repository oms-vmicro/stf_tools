#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "stf_exception.hpp"

class STFAddressRange {
    private:
        uint64_t start_pc_;
        uint64_t end_pc_;

    public:
        STFAddressRange(const uint64_t start_pc, const uint64_t end_pc) :
            start_pc_(start_pc),
            end_pc_(end_pc)
        {
            stf_assert(start_pc_ < end_pc_, "End PC must be larger than start PC");
        }

        inline uint64_t startAddress() const {
            return start_pc_;
        }

        inline uint64_t endAddress() const {
            return end_pc_;
        }

        inline uint64_t range() const {
            return end_pc_ - start_pc_;
        }

        inline bool contains(const uint64_t pc) const {
            return start_pc_ <= pc && pc < end_pc_;
        }

        inline bool startsBefore(const STFAddressRange& rhs) const {
            return start_pc_ < rhs.start_pc_;
        }

        inline bool startsAfter(const STFAddressRange& rhs) const {
            return start_pc_ > rhs.start_pc_;
        }

        inline bool operator<(const STFAddressRange& rhs) const {
            return startsBefore(rhs) || ((start_pc_ == rhs.start_pc_) && (range() < rhs.range()));
        }

        inline bool operator<(const uint64_t pc) const {
            return start_pc_ < pc;
        }

        inline bool operator>(const STFAddressRange& rhs) const {
            return startsAfter(rhs) || ((start_pc_ == rhs.start_pc_) && (range() > rhs.range()));
        }

        inline bool operator>(const uint64_t pc) const {
            return start_pc_ > pc;
        }

        inline bool operator==(const STFAddressRange& rhs) const {
            return (start_pc_ == rhs.start_pc_) && (end_pc_ == rhs.end_pc_);
        }

        inline bool operator<=(const STFAddressRange& rhs) const {
            return (*this < rhs) || (*this == rhs);
        }

        inline bool operator>=(const STFAddressRange& rhs) const {
            return (*this > rhs) || (*this == rhs);
        }
};
