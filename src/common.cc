#include "common.h"
#include "fmt/format.h"
#include <sstream>
#include <unordered_set>
#include <sys/stat.h>
#include <math.h>

namespace dramsim3 {

std::ostream& operator<<(std::ostream& os, const Command& cmd) {
    std::vector<std::string> command_string = {
        "read",
        "read_p",
        "write",
        "write_p",
        "activate",
        "precharge",
        "refresh_bank",  // verilog model doesn't distinguish bank/rank refresh
        "refresh",
        "self_refresh_enter",
        "self_refresh_exit",
        "lh_read",
        "lh_read_p",
        "gh_read",
        "gh_read_p",
        "pim_write",
        "pim_write_p",
        "pim_activate",
        "WRONG"};
    os << fmt::format("{:<20} {:>3} {:>3} {:>3} {:>3} {:>#8x} {:>#8x}",
                      command_string[static_cast<int>(cmd.cmd_type)],
                      cmd.Channel(), cmd.Rank(), cmd.Bankgroup(), cmd.Bank(),
                      cmd.Row(), cmd.Column());
    return os;
}

std::ostream& operator<<(std::ostream& os, const Transaction& trans) {
    const std::string trans_type = trans.is_pim ? "PIM" : trans.is_write ? "WRITE" : "READ";
    os << fmt::format("{:<30} {:>8}", trans.addr, trans_type);
    return os;
}

std::istream& operator>>(std::istream& is, Transaction& trans) {
    std::unordered_set<std::string> write_types = {"WRITE", "write", "P_MEM_WR",
                                                   "BOFF"};
    std::unordered_set<std::string> pim_types = {"PIM"};
    std::string mem_op;
    is >> std::hex >> trans.addr >> mem_op >> std::dec >> trans.added_cycle;
    // std::cout<<"Transaction being read: "<<std::hex<<trans.addr<<'\t'<<mem_op<<std::dec<<'\t'<<trans.added_cycle<<'\n';
    int w_cnt = write_types.count(mem_op);
    int pim_cnt = pim_types.count(mem_op);
    if (w_cnt == 0 && pim_cnt == 0)
        trans.active = false;
    else
        trans.active = true;
    trans.is_write = w_cnt == 1;
    trans.is_pim = pim_cnt == 1;


    return is;
}

int GetBitInPos(uint64_t bits, int pos) {
    // given a uint64_t value get the binary value of pos-th bit
    // from MSB to LSB indexed as 63 - 0
    return (bits >> pos) & 1;
}

int LogBase2(int power_of_two) {
    int i = 0;
    while (power_of_two > 1) {
        power_of_two /= 2;
        i++;
    }
    return i;
}

std::vector<std::string> StringSplit(const std::string& s, char delim) {
    std::vector<std::string> elems;
    StringSplit(s, delim, std::back_inserter(elems));
    return elems;
}

template <typename Out>
void StringSplit(const std::string& s, char delim, Out result) {
    std::stringstream ss;
    ss.str(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty()) {
            *(result++) = item;
        }
    }
}

void AbruptExit(const std::string& file, int line) {
    std::cerr << "Exiting Abruptly - " << file << ":" << line << std::endl;
    std::exit(-1);
}

bool DirExist(std::string dir) {
    // courtesy to stackoverflow
    struct stat info;
    if (stat(dir.c_str(), &info) != 0) {
        return false;
    } else if (info.st_mode & S_IFDIR) {
        return true;
    } else {  // exists but is file
        return false;
    }
}

}  // namespace dramsim3
